/*
 * src/bin/pgcopydb/ld_apply.c
 *     Implementation of a CLI to copy a database between two Postgres instances
 */

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <sys/wait.h>
#include <unistd.h>

#include "postgres.h"
#include "postgres_fe.h"
#include "access/xlog_internal.h"
#include "access/xlogdefs.h"

#include "parson.h"

#include "cli_common.h"
#include "cli_root.h"
#include "copydb.h"
#include "env_utils.h"
#include "ld_stream.h"
#include "lock_utils.h"
#include "log.h"
#include "parsing_utils.h"
#include "pidfile.h"
#include "pg_utils.h"
#include "schema.h"
#include "signals.h"
#include "string_utils.h"
#include "summary.h"


/*
 * stream_apply_catchup catches up with SQL files that have been prepared by
 * either the `pgcopydb stream prefetch` command. If an endpos
 */
bool
stream_apply_catchup(StreamSpecs *specs)
{
	StreamApplyContext context = { 0 };

	/* in prefetch mode, wait until the sentinel enables the apply process */
	if (specs->mode == STREAM_MODE_PREFETCH)
	{
		if (!stream_apply_wait_for_sentinel(specs, &context))
		{
			/* errors have already been logged */
			return false;
		}
	}

	if (!stream_read_context(&(specs->paths),
							 &(context.system),
							 &(context.WalSegSz)))
	{
		log_error("Failed to read the streaming context information "
				  "from the source database, see above for details");
		return false;
	}

	log_debug("Source database wal_segment_size is %u", context.WalSegSz);
	log_debug("Source database timeline is %d", context.system.timeline);

	if (!setupReplicationOrigin(&context,
								&(specs->paths),
								specs->source_pguri,
								specs->target_pguri,
								specs->origin,
								specs->endpos,
								context.apply))
	{
		log_error("Failed to setup replication origin on the target database");
		return false;
	}

	log_info("Catching up from LSN %X/%X in \"%s\"",
			 LSN_FORMAT_ARGS(context.previousLSN),
			 context.sqlFileName);

	if (context.endpos != InvalidXLogRecPtr)
	{
		log_info("Stopping at endpos LSN %X/%X",
				 LSN_FORMAT_ARGS(context.endpos));
	}

	/*
	 * Our main loop reads the current SQL file, applying all the queries from
	 * there and tracking progress, and then goes on to the next file, until no
	 * such file exists.
	 */
	char currentSQLFileName[MAXPGPATH] = { 0 };

	for (;;)
	{
		strlcpy(currentSQLFileName, context.sqlFileName, MAXPGPATH);

		if (asked_to_stop || asked_to_stop_fast || asked_to_quit)
		{
			break;
		}

		/*
		 * It might be the expected file doesn't exist already, in that case
		 * continue looping until the concurrent prefetch mechanism has created
		 * it.
		 */
		if (!file_exists(context.sqlFileName))
		{
			log_debug("File \"%s\" does not exists yet, retrying in %dms",
					  context.sqlFileName,
					  CATCHINGUP_SLEEP_MS);

			pg_usleep(CATCHINGUP_SLEEP_MS * 1000);
			continue;
		}

		/*
		 * The SQL file exists already, apply it now.
		 */
		if (!stream_apply_file(&context))
		{
			/* errors have already been logged */
			return false;
		}

		/*
		 * Each time we are done applying a file, we update our progress and
		 * fetch new values from the pgcopydb sentinel. Errors are warning
		 * here, we'll update next time.
		 */
		(void) stream_apply_sync_sentinel(&context);

		/*
		 * When syncing with the pgcopydb sentinel we might receive a new
		 * endpos, and it might mean we're done already.
		 */
		if (!context.reachedEndPos &&
			context.endpos != InvalidXLogRecPtr &&
			context.endpos <= context.previousLSN)
		{
			context.reachedEndPos = true;

			log_info("Applied reached end position %X/%X at %X/%X",
					 LSN_FORMAT_ARGS(context.endpos),
					 LSN_FORMAT_ARGS(context.previousLSN));
		}

		if (context.reachedEndPos)
		{
			/* information has already been logged */
			break;
		}

		if (!computeSQLFileName(&context))
		{
			/* errors have already been logged */
			return false;
		}

		if (strcmp(context.sqlFileName, currentSQLFileName) == 0)
		{
			log_debug("Reached end of file \"%s\" at %X/%X.",
					  currentSQLFileName,
					  LSN_FORMAT_ARGS(context.previousLSN));

			/*
			 * Sleep for a while (10s typically) then try again, new data might
			 * have been appended to the same file again.
			 */
			pg_usleep(CATCHINGUP_SLEEP_MS * 1000);
		}
	}

	/* we might still have to disconnect now */
	(void) pgsql_finish(&(context.pgsql));

	return true;
}


/*
 * stream_apply_wait_for_sentinel fetches the current pgcopydb sentinel values
 * on the source database: the catchup processing only gets to start when the
 * sentinel "apply" column has been set to true.
 */
bool
stream_apply_wait_for_sentinel(StreamSpecs *specs, StreamApplyContext *context)
{
	PGSQL src = { 0 };
	bool firstLoop = true;
	CopyDBSentinel sentinel = { 0 };

	if (!pgsql_init(&src, specs->source_pguri, PGSQL_CONN_SOURCE))
	{
		/* errors have already been logged */
		return false;
	}

	while (!sentinel.apply)
	{
		if (asked_to_stop || asked_to_stop_fast || asked_to_quit)
		{
			log_info("Apply process received a shutdown signal "
					 "while waiting for apply mode, "
					 "quitting now");
			return false;
		}

		/* this reconnects on each loop iteration, every 10s by default */
		if (!pgsql_get_sentinel(&src, &sentinel))
		{
			log_warn("Retrying to fetch pgcopydb sentinel values in %ds",
					 CATCHINGUP_SLEEP_MS / 10);
			pg_usleep(CATCHINGUP_SLEEP_MS * 1000);

			continue;
		}

		log_debug("startpos %X/%X endpos %X/%X apply %s",
				  LSN_FORMAT_ARGS(sentinel.startpos),
				  LSN_FORMAT_ARGS(sentinel.endpos),
				  sentinel.apply ? "enabled" : "disabled");

		if (sentinel.apply)
		{
			context->startpos = sentinel.startpos;
			context->endpos = sentinel.endpos;
			context->apply = sentinel.apply;

			break;
		}

		if (firstLoop)
		{
			firstLoop = false;

			log_info("Waiting until the pgcopydb sentinel apply is enabled");
		}

		/* avoid buzy looping and avoid hammering the source database */
		pg_usleep(CATCHINGUP_SLEEP_MS * 1000);
	}

	log_info("The pgcopydb sentinel has enabled applying changes");

	return true;
}


/*
 * stream_apply_sync_sentinel sync with the pgcopydb sentinel table, sending
 * the current replay LSN position and fetching the maybe new endpos and apply
 * values.
 */
bool
stream_apply_sync_sentinel(StreamApplyContext *context)
{
	PGSQL src = { 0 };
	CopyDBSentinel sentinel = { 0 };

	if (!pgsql_init(&src, context->source_pguri, PGSQL_CONN_SOURCE))
	{
		/* errors have already been logged */
		return false;
	}

	if (!pgsql_sync_sentinel_apply(&src, context->previousLSN, &sentinel))
	{
		log_warn("Failed to sync progress with the pgcopydb sentinel");
	}

	context->apply = sentinel.apply;
	context->endpos = sentinel.endpos;
	context->startpos = sentinel.startpos;

	return true;
}


/*
 * stream_apply_file connects to the target database system and applies the
 * given SQL file as prepared by the stream_transform_file function.
 */
bool
stream_apply_file(StreamApplyContext *context)
{
	StreamContent content = { 0 };
	long size = 0L;

	strlcpy(content.filename, context->sqlFileName, sizeof(content.filename));

	if (!read_file(content.filename, &(content.buffer), &size))
	{
		/* errors have already been logged */
		return false;
	}

	content.count = countLines(content.buffer);
	content.lines = (char **) calloc(content.count, sizeof(char *));
	content.count = splitLines(content.buffer, content.lines, content.count);

	if (content.lines == NULL)
	{
		log_error(ALLOCATION_FAILED_ERROR);
		return false;
	}

	log_info("Replaying changes from file \"%s\"", context->sqlFileName);

	log_debug("Read %d lines in file \"%s\"",
			  content.count,
			  content.filename);

	PGSQL *pgsql = &(context->pgsql);
	bool reachedStartingPosition = false;

	/* replay the SQL commands from the SQL file */
	for (int i = 0; i < content.count && !context->reachedEndPos; i++)
	{
		const char *sql = content.lines[i];
		LogicalMessageMetadata metadata = { 0 };

		StreamAction action = parseSQLAction(sql, &metadata);

		switch (action)
		{
			case STREAM_ACTION_SWITCH:
			{
				/*
				 * The SWITCH WAL command should always be the last line of the
				 * file...
				 */
				if (i != (content.count - 1))
				{
					log_error("SWITCH command found in line %d, "
							  "before last line %d",
							  i + 1,
							  content.count);
					return false;
				}

				log_debug("apply: SWITCH from %X/%X to %X/%X",
						  LSN_FORMAT_ARGS(context->previousLSN),
						  LSN_FORMAT_ARGS(metadata.lsn));

				context->previousLSN = metadata.lsn;

				break;
			}

			case STREAM_ACTION_BEGIN:
			{
				/* did we reach the starting LSN positions now? */
				if (!reachedStartingPosition)
				{
					reachedStartingPosition =
						context->previousLSN < metadata.lsn;
				}

				log_debug("BEGIN %lld LSN %X/%X @%s, previous LSN %X/%X %s",
						  (long long) metadata.xid,
						  LSN_FORMAT_ARGS(metadata.lsn),
						  metadata.timestamp,
						  LSN_FORMAT_ARGS(context->previousLSN),
						  reachedStartingPosition ? "" : "[skipping]");

				if (metadata.lsn == InvalidXLogRecPtr ||
					IS_EMPTY_STRING_BUFFER(metadata.timestamp))
				{
					log_fatal("Failed to parse BEGIN message: %s", sql);
					return false;
				}

				/*
				 * Check if we reached the endpos LSN already.
				 */
				if (context->endpos != InvalidXLogRecPtr &&
					context->endpos <= metadata.lsn)
				{
					context->reachedEndPos = true;

					log_info("Apply reached end position %X/%X at %X/%X.",
							 LSN_FORMAT_ARGS(context->endpos),
							 LSN_FORMAT_ARGS(metadata.lsn));
					break;
				}

				/* actually skip this one if we didn't reach start pos yet */
				if (!reachedStartingPosition)
				{
					continue;
				}

				/*
				 * We're all good to replay that transaction, let's BEGIN and
				 * register our origin tracking on the target database.
				 */
				if (!pgsql_begin(pgsql))
				{
					/* errors have already been logged */
					return false;
				}

				char lsn[PG_LSN_MAXLENGTH] = { 0 };

				sformat(lsn, sizeof(lsn), "%X/%X",
						LSN_FORMAT_ARGS(metadata.lsn));

				if (!pgsql_replication_origin_xact_setup(pgsql,
														 lsn,
														 metadata.timestamp))
				{
					/* errors have already been logged */
					return false;
				}

				break;
			}

			case STREAM_ACTION_COMMIT:
			{
				if (!reachedStartingPosition)
				{
					continue;
				}

				log_debug("COMMIT %lld LSN %X/%X",
						  (long long) metadata.xid,
						  LSN_FORMAT_ARGS(metadata.lsn));


				/* calling pgsql_commit() would finish the connection, avoid */
				if (!pgsql_execute(pgsql, "COMMIT"))
				{
					/* errors have already been logged */
					return false;
				}

				context->previousLSN = metadata.lsn;

				/*
				 * At COMMIT time we might have reached the endpos: we know
				 * that already when endpos <= lsn. It's important to check
				 * that at COMMIT record time, because that record might be the
				 * last entry of the file we're applying.
				 */
				if (context->endpos != InvalidXLogRecPtr &&
					context->endpos <= context->previousLSN)
				{
					context->reachedEndPos = true;

					log_info("Applied reached end position %X/%X at %X/%X",
							 LSN_FORMAT_ARGS(context->endpos),
							 LSN_FORMAT_ARGS(context->previousLSN));
					break;
				}

				break;
			}

			/*
			 * A KEEPALIVE message is replayed as its own transaction where the
			 * only thgin we do is call into the replication origin tracking
			 * API to advance our position on the target database.
			 */
			case STREAM_ACTION_KEEPALIVE:
			{
				/* did we reach the starting LSN positions now? */
				if (!reachedStartingPosition)
				{
					reachedStartingPosition =
						context->previousLSN < metadata.lsn;
				}

				log_debug("KEEPALIVE LSN %X/%X @%s, previous LSN %X/%X %s",
						  LSN_FORMAT_ARGS(metadata.lsn),
						  metadata.timestamp,
						  LSN_FORMAT_ARGS(context->previousLSN),
						  reachedStartingPosition ? "" : "[skipping]");

				if (metadata.lsn == InvalidXLogRecPtr ||
					IS_EMPTY_STRING_BUFFER(metadata.timestamp))
				{
					log_fatal("Failed to parse KEEPALIVE message: %s", sql);
					return false;
				}

				/*
				 * Check if we reached the endpos LSN already. If the keepalive
				 * message is the endpos, still apply it: its only purpose is
				 * to maintain our replication origin tracking on the target
				 * database.
				 */
				if (context->endpos != InvalidXLogRecPtr &&
					context->endpos < metadata.lsn)
				{
					context->reachedEndPos = true;

					log_info("Apply reached end position %X/%X at %X/%X.",
							 LSN_FORMAT_ARGS(context->endpos),
							 LSN_FORMAT_ARGS(metadata.lsn));
					break;
				}

				/* actually skip this one if we didn't reach start pos yet */
				if (!reachedStartingPosition)
				{
					continue;
				}

				if (!pgsql_begin(pgsql))
				{
					/* errors have already been logged */
					return false;
				}

				char lsn[PG_LSN_MAXLENGTH] = { 0 };

				sformat(lsn, sizeof(lsn), "%X/%X",
						LSN_FORMAT_ARGS(metadata.lsn));

				if (!pgsql_replication_origin_xact_setup(pgsql,
														 lsn,
														 metadata.timestamp))
				{
					/* errors have already been logged */
					return false;
				}

				/* calling pgsql_commit() would finish the connection, avoid */
				if (!pgsql_execute(pgsql, "COMMIT"))
				{
					/* errors have already been logged */
					return false;
				}

				context->previousLSN = metadata.lsn;

				/*
				 * At COMMIT time we might have reached the endpos: we know
				 * that already when endpos <= lsn. It's important to check
				 * that at COMMIT record time, because that record might be the
				 * last entry of the file we're applying.
				 */
				if (context->endpos != InvalidXLogRecPtr &&
					context->endpos <= context->previousLSN)
				{
					context->reachedEndPos = true;

					log_info("Applied reached end position %X/%X at %X/%X",
							 LSN_FORMAT_ARGS(context->endpos),
							 LSN_FORMAT_ARGS(context->previousLSN));
					break;
				}

				break;
			}

			case STREAM_ACTION_INSERT:
			case STREAM_ACTION_UPDATE:
			case STREAM_ACTION_DELETE:
			case STREAM_ACTION_TRUNCATE:
			{
				if (!reachedStartingPosition)
				{
					continue;
				}

				/* chomp the final semi-colon that we added */
				int len = strlen(sql);

				if (sql[len - 1] == ';')
				{
					char *ptr = (char *) sql + len - 1;
					*ptr = '\0';
				}

				if (!pgsql_execute(pgsql, sql))
				{
					/* errors have already been logged */
					return false;
				}
				break;
			}

			default:
			{
				log_error("Failed to parse SQL query \"%s\"", sql);
				return false;
			}
		}
	}

	/* free dynamic memory that's not needed anymore */
	free(content.buffer);
	free(content.lines);

	return true;
}


/*
 * setupReplicationOrigin ensures that a replication origin has been created on
 * the target database, and if it has been created previously then fetches the
 * previous LSN position it was at.
 *
 * Also setupReplicationOrigin calls pg_replication_origin_setup() in the
 * current connection, that is set to be
 */
bool
setupReplicationOrigin(StreamApplyContext *context,
					   CDCPaths *paths,
					   char *source_pguri,
					   char *target_pguri,
					   char *origin,
					   uint64_t endpos,
					   bool apply)
{
	PGSQL *pgsql = &(context->pgsql);
	char *nodeName = context->origin;

	/*
	 * We have to consider both the --endpos command line option and the
	 * pgcopydb sentinel endpos value. Typically the sentinel is updated after
	 * the fact, but we still give precedence to --endpos.
	 *
	 * The endpos parameter here comes from the --endpos command line option,
	 * the context->endpos might have been set by calling
	 * stream_apply_wait_for_sentinel() earlier (when in STREAM_MODE_PREFETCH).
	 */
	if (endpos != InvalidXLogRecPtr)
	{
		if (context->endpos != InvalidXLogRecPtr)
		{
			log_warn("Option --endpos %X/%X is used, "
					 "even when the pgcopydb sentinel endpos is set to %X/%X",
					 LSN_FORMAT_ARGS(endpos),
					 LSN_FORMAT_ARGS(context->endpos));
		}
		context->endpos = endpos;
	}

	context->paths = *paths;
	context->apply = apply;
	strlcpy(context->source_pguri, source_pguri, sizeof(context->source_pguri));
	strlcpy(context->target_pguri, target_pguri, sizeof(context->target_pguri));
	strlcpy(context->origin, origin, sizeof(context->origin));

	if (!pgsql_init(pgsql, context->target_pguri, PGSQL_CONN_TARGET))
	{
		/* errors have already been logged */
		return false;
	}

	/* we're going to send several replication origin commands */
	pgsql->connectionStatementType = PGSQL_CONNECTION_MULTI_STATEMENT;

	uint32_t oid = 0;

	if (!pgsql_replication_origin_oid(pgsql, nodeName, &oid))
	{
		/* errors have already been logged */
		return false;
	}

	log_debug("setupReplicationOrigin: oid == %u", oid);

	if (oid == 0)
	{
		log_error("Failed to fetch progress for replication origin \"%s\": "
				  "replication origin not found on target database",
				  nodeName);
		(void) pgsql_finish(pgsql);
		return false;
	}

	if (!pgsql_replication_origin_progress(pgsql,
										   nodeName,
										   true,
										   &(context->previousLSN)))
	{
		/* errors have already been logged */
		return false;
	}

	if (!computeSQLFileName(context))
	{
		/* errors have already been logged */
		return false;
	}

	/* compute the WAL filename that would host the previous LSN */
	log_debug("setupReplicationOrigin: replication origin \"%s\" "
			  "found at %X/%X, expected in file \"%s\"",
			  nodeName,
			  LSN_FORMAT_ARGS(context->previousLSN),
			  context->sqlFileName);

	if (!pgsql_replication_origin_session_setup(pgsql, nodeName))
	{
		/* errors have already been logged */
		return false;
	}

	return true;
}


/*
 * computeSQLFileName updates the StreamApplyContext structure with the current
 * LSN applied to the target system, and computed
 */
bool
computeSQLFileName(StreamApplyContext *context)
{
	XLogSegNo segno;

	XLByteToSeg(context->previousLSN, segno, context->WalSegSz);
	XLogFileName(context->wal, context->system.timeline, segno, context->WalSegSz);

	sformat(context->sqlFileName, sizeof(context->sqlFileName),
			"%s/%s.sql",
			context->paths.dir,
			context->wal);

	log_debug("computeSQLFileName: %X/%X \"%s\"",
			  LSN_FORMAT_ARGS(context->previousLSN),
			  context->sqlFileName);

	return true;
}


/*
 * parseSQLAction returns the action that is implemented in the given SQL
 * query.
 */
StreamAction
parseSQLAction(const char *query, LogicalMessageMetadata *metadata)
{
	StreamAction action = STREAM_ACTION_UNKNOWN;

	if (strcmp(query, "") == 0)
	{
		return action;
	}

	char *message = NULL;
	char *begin = strstr(query, OUTPUT_BEGIN);
	char *commit = strstr(query, OUTPUT_COMMIT);
	char *switchwal = strstr(query, OUTPUT_SWITCHWAL);
	char *keepalive = strstr(query, OUTPUT_KEEPALIVE);

	/* do we have a BEGIN or a COMMIT message to parse metadata of? */
	if (query == begin)
	{
		action = STREAM_ACTION_BEGIN;
		message = begin + strlen(OUTPUT_BEGIN);
	}
	else if (query == commit)
	{
		action = STREAM_ACTION_COMMIT;
		message = commit + strlen(OUTPUT_BEGIN);
	}
	else if (query == switchwal)
	{
		action = STREAM_ACTION_SWITCH;
		message = switchwal + strlen(OUTPUT_SWITCHWAL);
	}
	else if (query == keepalive)
	{
		action = STREAM_ACTION_KEEPALIVE;
		message = keepalive + strlen(OUTPUT_KEEPALIVE);
	}

	if (message != NULL)
	{
		JSON_Value *json = json_parse_string(message);

		metadata->action = action;

		if (!parseMessageMetadata(metadata, message, json, true))
		{
			/* errors have already been logged */
			json_value_free(json);
			return false;
		}

		json_value_free(json);
	}

	if (strstr(query, "INSERT INTO") != NULL)
	{
		action = STREAM_ACTION_INSERT;
	}
	else if (strstr(query, "UPDATE ") != NULL)
	{
		action = STREAM_ACTION_UPDATE;
	}
	else if (strstr(query, "DELETE FROM ") != NULL)
	{
		action = STREAM_ACTION_DELETE;
	}
	else if (strstr(query, "TRUNCATE ") != NULL)
	{
		action = STREAM_ACTION_TRUNCATE;
	}

	return action;
}