/*-------------------------------------------------------------------------
 *
 * pg_rat_spa.c
 *		PostgreSQL Real Application Testing - SQL Performance Analyzer
 *
 * Implements per-query performance analysis by re-executing distinct
 * queries from the capture set on the target database using SPI with
 * EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON), then storing the
 * source-vs-target performance comparison in pg_rat_spa_results.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/pg_rat/pg_rat_spa.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <dirent.h>
#include <math.h>

#include "executor/spi.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"

#include "pg_rat.h"

/* Maximum unique queries to analyze */
#define RAT_SPA_MAX_QUERIES		10000

/* Hash table entry for unique query deduplication */
typedef struct SpaQueryEntry
{
	char		query_text[RAT_MAX_QUERY_LEN]; /* hash key */
	double		source_elapsed_ms;			/* original duration */
	int64		source_rows;
	int64		source_blks_hit;
	int64		source_blks_read;
	int			exec_count;					/* number of executions in capture */
} SpaQueryEntry;

/*
 * Simple hash function for query text.
 */
static uint32
spa_query_hash(const char *query, int len)
{
	uint32		h = 5381;
	int			i;

	for (i = 0; i < len; i++)
		h = ((h << 5) + h) + (unsigned char) query[i];

	return h;
}

/*
 * Parse a JSON field from EXPLAIN output.
 * Looks for "field": <number> pattern.
 */
static double
extract_explain_field(const char *json, const char *field)
{
	char		search[128];
	const char *pos;

	snprintf(search, sizeof(search), "\"%s\":", field);
	pos = strstr(json, search);
	if (!pos)
		return -1.0;

	pos += strlen(search);
	while (*pos == ' ')
		pos++;

	return strtod(pos, NULL);
}

PG_FUNCTION_INFO_V1(pg_rat_run_spa);

/*
 * pg_rat_run_spa(capture_dir text, comparison_metric text) → void
 *
 * Reads NDJSON capture files, builds a unique query set based on hash,
 * then for each unique query:
 *   1. Runs EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) via SPI
 *   2. Extracts planning/execution times and buffer stats
 *   3. Inserts comparison results into pg_rat_spa_results
 */
Datum
pg_rat_run_spa(PG_FUNCTION_ARGS)
{
	text	   *capture_dir_text = PG_GETARG_TEXT_PP(0);
	text	   *metric_text = PG_GETARG_TEXT_PP(1);
	const char *capture_name = text_to_cstring(capture_dir_text);
	(void) metric_text; /* unused for now */
	char		dirpath[MAXPGPATH];
	DIR		   *dir;
	struct dirent *de;

	int			nqueries = 0;
	HTAB	   *query_hash;
	HASHCTL		hash_info;

	if (!rat_shared_state)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_rat must be loaded via shared_preload_libraries")));

	snprintf(dirpath, MAXPGPATH, "%s/%s",
			 rat_capture_directory, capture_name);

	/* Create local hash table for query deduplication */
	memset(&hash_info, 0, sizeof(hash_info));
	hash_info.keysize = RAT_MAX_QUERY_LEN;
	hash_info.entrysize = sizeof(SpaQueryEntry);
	query_hash = hash_create("pg_rat SPA queries",
							 RAT_SPA_MAX_QUERIES,
							 &hash_info,
							 HASH_ELEM | HASH_BLOBS);

	/* Read and deduplicate queries from NDJSON files */
	dir = opendir(dirpath);
	if (dir == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open capture directory \"%s\": %m",
						dirpath)));

	while ((de = readdir(dir)) != NULL)
	{
		char		filepath[MAXPGPATH];
		FILE	   *fp;
		char		line[RAT_MAX_QUERY_LEN * 2 + 512];

		if (strstr(de->d_name, ".ndjson") == NULL)
			continue;

		snprintf(filepath, MAXPGPATH, "%s/%s", dirpath, de->d_name);
		fp = fopen(filepath, "r");
		if (fp == NULL)
			continue;

		while (fgets(line, sizeof(line), fp) != NULL)
		{
			char		query[RAT_MAX_QUERY_LEN];
			char		type_str[32];
			int64		rows, blk_hit, blk_read;
			double		dur_ms;
			uint32		qhash;
			bool		found;
			SpaQueryEntry *entry;
			const char *dur_pos;

			/* Only analyze QUERY_END events */
			{
				char		search_key[32];
				const char *type_pos;
				int j = 0;

				snprintf(search_key, sizeof(search_key), "\"type\":\"");
				type_pos = strstr(line, search_key);
				if (!type_pos)
					continue;
				type_pos += strlen(search_key);
				while (*type_pos && *type_pos != '"' && j < 31)
					type_str[j++] = *type_pos++;
				type_str[j] = '\0';
			}

			if (strcmp(type_str, "QUERY_END") != 0)
				continue;

			/* Extract query text */
			{
				const char *q_pos = strstr(line, "\"query\":\"");
				int j = 0;

				if (!q_pos)
					continue;
				q_pos += 9;
				while (*q_pos && j < RAT_MAX_QUERY_LEN - 1)
				{
					if (*q_pos == '\\' && *(q_pos + 1))
					{
						q_pos++;
						switch (*q_pos)
						{
							case '"':
								query[j++] = '"';
								break;
							case '\\':
								query[j++] = '\\';
								break;
							case 'n':
								query[j++] = '\n';
								break;
							case 'r':
								query[j++] = '\r';
								break;
							case 't':
								query[j++] = '\t';
								break;
							default:
								query[j++] = *q_pos;
								break;
						}
					}
					else if (*q_pos == '"')
					{
						break;
					}
					else
					{
						query[j++] = *q_pos;
					}
					q_pos++;
				}
				query[j] = '\0';
			}

			if (strlen(query) == 0)
				continue;

			/* Extract duration */
			dur_pos = strstr(line, "\"dur_ms\":");
			dur_ms = dur_pos ? strtod(dur_pos + 9, NULL) : 0.0;

			/* Extract buffer stats */
			{
				const char *p;
				p = strstr(line, "\"shblk_hit\":");
				blk_hit = p ? strtoll(p + 12, NULL, 10) : 0;
				p = strstr(line, "\"shblk_read\":");
				blk_read = p ? strtoll(p + 13, NULL, 10) : 0;
				p = strstr(line, "\"rows\":");
				rows = p ? strtoll(p + 7, NULL, 10) : 0;
			}

			/* Use query text directly as hash key */
			char search_key[RAT_MAX_QUERY_LEN];
			MemSet(search_key, 0, RAT_MAX_QUERY_LEN);
			strlcpy(search_key, query, RAT_MAX_QUERY_LEN);

			entry = (SpaQueryEntry *) hash_search(query_hash, search_key,
												  HASH_ENTER, &found);
			if (!found)
			{
				entry->source_elapsed_ms = dur_ms;
				entry->source_rows = rows;
				entry->source_blks_hit = blk_hit;
				entry->source_blks_read = blk_read;
				entry->exec_count = 1;
				nqueries++;
			}
			else
			{
				/* Aggregate: keep average for duration, sum for buffers */
				entry->source_elapsed_ms =
					(entry->source_elapsed_ms * entry->exec_count + dur_ms) /
					(entry->exec_count + 1);
				entry->source_blks_hit += blk_hit;
				entry->source_blks_read += blk_read;
				entry->exec_count++;
			}
		}
		fclose(fp);
	}
	closedir(dir);

	elog(LOG, "pg_rat: SPA analyzing %d unique queries", nqueries);

	/* Connect to SPI for EXPLAIN ANALYZE and result insertion */
	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("pg_rat: SPI_connect failed")));

	/* Ensure the results table exists */
	SPI_execute("CREATE TABLE IF NOT EXISTS pg_rat_spa_results ("
				"query_hash bigint, "
				"query_text text, "
				"source_elapsed_ms double precision, "
				"target_elapsed_ms double precision, "
				"source_buffers bigint, "
				"target_buffers bigint, "
				"regression_pct double precision, "
				"plan_changed boolean, "
				"target_plan_json text, "
				"exec_count integer"
				")", false, 0);

	/* Truncate previous results */
	SPI_execute("TRUNCATE pg_rat_spa_results", false, 0);

	/* Iterate over unique queries and EXPLAIN ANALYZE each one */
	{
		HASH_SEQ_STATUS seq;
		SpaQueryEntry *entry;

		hash_seq_init(&seq, query_hash);
		while ((entry = (SpaQueryEntry *) hash_seq_search(&seq)) != NULL)
		{
			StringInfoData explain_query;
			StringInfoData insert_query;
			double		target_elapsed = 0.0;
			int64		target_buffers = 0;
			const char *plan_json = "";
			double		regression_pct = 0.0;
			int			ret;

			/* Build EXPLAIN query */
			initStringInfo(&explain_query);
			appendStringInfo(&explain_query,
							 "EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) %s",
							 entry->query_text);

			/* Execute EXPLAIN ANALYZE - catch errors gracefully */
			/* Restrict to SELECT queries for safety to prevent DML mutation */
			if (strncasecmp(entry->query_text, "SELECT", 6) != 0)
			{
				target_elapsed = -1.0;
				goto skip_explain;
			}
			PG_TRY();
			{
				ret = SPI_execute(explain_query.data, true, 0);
				if (ret == SPI_OK_SELECT && SPI_tuptable != NULL &&
					SPI_processed > 0)
				{
					char	   *result;

					result = SPI_getvalue(SPI_tuptable->vals[0],
										  SPI_tuptable->tupdesc, 1);
					if (result)
					{
						/* Extract Execution Time and buffer stats */
						target_elapsed = extract_explain_field(result, "Execution Time");
						if (target_elapsed < 0)
							target_elapsed = extract_explain_field(result, "Total Cost");

						{
							double hits = extract_explain_field(result, "Shared Hit Blocks");
							double reads = extract_explain_field(result, "Shared Read Blocks");
							if (hits >= 0)
								target_buffers = (int64) hits;
							if (reads >= 0)
								target_buffers += (int64) reads;
						}

						plan_json = result;
					}
				}
			}
			PG_CATCH();
			{
				/* Query failed on target — mark as error */
				FlushErrorState();
				target_elapsed = -1.0;
			}
			PG_END_TRY();
skip_explain:

			/* Compute regression percentage */
			if (entry->source_elapsed_ms > 0 && target_elapsed > 0)
				regression_pct = ((target_elapsed - entry->source_elapsed_ms) /
								  entry->source_elapsed_ms) * 100.0;

			/* Insert result row */
			initStringInfo(&insert_query);
			appendStringInfo(&insert_query,
							 "INSERT INTO pg_rat_spa_results VALUES "
							 "(%u, $1, %f, %f, %lld, "
							 "%lld, %f, %s, $2, %d)",
							 spa_query_hash(entry->query_text, strlen(entry->query_text)),
							 entry->source_elapsed_ms,
							 target_elapsed,
							 (long long) (entry->source_blks_hit + entry->source_blks_read),
							 (long long) target_buffers,
							 regression_pct,
							 fabs(regression_pct) > 10.0 ? "true" : "false",
							 entry->exec_count);

			{
				Oid			argtypes[2] = {TEXTOID, TEXTOID};
				Datum		argvals[2];
				char		nullsarg[2] = {' ', ' '};

				argvals[0] = CStringGetTextDatum(entry->query_text);
				argvals[1] = CStringGetTextDatum(plan_json);

				SPI_execute_with_args(insert_query.data, 2, argtypes,
									  argvals, nullsarg, false, 0);
			}

			pfree(explain_query.data);
			pfree(insert_query.data);
		}
	}

	SPI_finish();

	elog(LOG, "pg_rat: SPA analysis complete — %d queries analyzed", nqueries);

	PG_RETURN_VOID();
}
