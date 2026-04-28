/*-------------------------------------------------------------------------
 *
 * pg_rat_report.c
 *		PostgreSQL Real Application Testing - HTML report generator
 *
 * Generates a modern, styled HTML regression report from the
 * pg_rat_spa_results table. The report includes:
 *   - Executive summary (total queries, regressions, improvements)
 *   - Sortable table with color-coded regression/improvement cells
 *   - Dark-themed modern CSS with responsive design
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/pg_rat/pg_rat_report.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "storage/fd.h"
#include "utils/builtins.h"

#include "pg_rat.h"

/* ----------------
 * HTML template fragments
 * ----------------
 */

static const char *report_html_header =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"<title>pg_rat — SQL Performance Analysis Report</title>\n"
"<style>\n"
"  :root {\n"
"    --bg-primary: #0f172a;\n"
"    --bg-secondary: #1e293b;\n"
"    --bg-card: #334155;\n"
"    --text-primary: #f1f5f9;\n"
"    --text-secondary: #94a3b8;\n"
"    --accent-blue: #3b82f6;\n"
"    --accent-green: #10b981;\n"
"    --accent-red: #ef4444;\n"
"    --accent-yellow: #f59e0b;\n"
"    --border: #475569;\n"
"  }\n"
"  * { margin: 0; padding: 0; box-sizing: border-box; }\n"
"  body {\n"
"    font-family: 'Inter', -apple-system, BlinkMacSystemFont, sans-serif;\n"
"    background: var(--bg-primary);\n"
"    color: var(--text-primary);\n"
"    line-height: 1.6;\n"
"    padding: 2rem;\n"
"  }\n"
"  .container { max-width: 1400px; margin: 0 auto; }\n"
"  .header {\n"
"    background: linear-gradient(135deg, #1e40af, #7c3aed);\n"
"    border-radius: 12px;\n"
"    padding: 2rem;\n"
"    margin-bottom: 2rem;\n"
"  }\n"
"  .header h1 {\n"
"    font-size: 1.8rem;\n"
"    font-weight: 700;\n"
"    margin-bottom: 0.5rem;\n"
"  }\n"
"  .header p { color: #c7d2fe; font-size: 0.95rem; }\n"
"  .stats-grid {\n"
"    display: grid;\n"
"    grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));\n"
"    gap: 1rem;\n"
"    margin-bottom: 2rem;\n"
"  }\n"
"  .stat-card {\n"
"    background: var(--bg-secondary);\n"
"    border-radius: 10px;\n"
"    padding: 1.5rem;\n"
"    border: 1px solid var(--border);\n"
"  }\n"
"  .stat-card .label { color: var(--text-secondary); font-size: 0.85rem; text-transform: uppercase; letter-spacing: 0.05em; }\n"
"  .stat-card .value { font-size: 2rem; font-weight: 700; margin-top: 0.25rem; }\n"
"  .stat-card .value.green { color: var(--accent-green); }\n"
"  .stat-card .value.red { color: var(--accent-red); }\n"
"  .stat-card .value.blue { color: var(--accent-blue); }\n"
"  .stat-card .value.yellow { color: var(--accent-yellow); }\n"
"  table {\n"
"    width: 100%;\n"
"    border-collapse: collapse;\n"
"    background: var(--bg-secondary);\n"
"    border-radius: 10px;\n"
"    overflow: hidden;\n"
"  }\n"
"  thead { background: var(--bg-card); }\n"
"  th {\n"
"    padding: 0.75rem 1rem;\n"
"    text-align: left;\n"
"    font-weight: 600;\n"
"    font-size: 0.8rem;\n"
"    text-transform: uppercase;\n"
"    letter-spacing: 0.05em;\n"
"    color: var(--text-secondary);\n"
"    cursor: pointer;\n"
"    user-select: none;\n"
"  }\n"
"  th:hover { color: var(--accent-blue); }\n"
"  td {\n"
"    padding: 0.6rem 1rem;\n"
"    border-top: 1px solid var(--border);\n"
"    font-size: 0.9rem;\n"
"    max-width: 500px;\n"
"    overflow: hidden;\n"
"    text-overflow: ellipsis;\n"
"    white-space: nowrap;\n"
"  }\n"
"  tr:hover { background: rgba(59, 130, 246, 0.08); }\n"
"  .regression { color: var(--accent-red); font-weight: 600; }\n"
"  .improvement { color: var(--accent-green); font-weight: 600; }\n"
"  .neutral { color: var(--text-secondary); }\n"
"  .query-text {\n"
"    font-family: 'Fira Code', 'JetBrains Mono', monospace;\n"
"    font-size: 0.8rem;\n"
"    color: var(--text-secondary);\n"
"  }\n"
"  .footer {\n"
"    text-align: center;\n"
"    color: var(--text-secondary);\n"
"    font-size: 0.8rem;\n"
"    margin-top: 2rem;\n"
"    padding: 1rem;\n"
"  }\n"
"  @media (max-width: 768px) {\n"
"    body { padding: 1rem; }\n"
"    td, th { padding: 0.4rem 0.6rem; font-size: 0.8rem; }\n"
"  }\n"
"</style>\n"
"<script>\n"
"function sortTable(col) {\n"
"  var t = document.getElementById('results');\n"
"  var rows = Array.from(t.tBodies[0].rows);\n"
"  var dir = t.dataset.sortDir === 'asc' ? -1 : 1;\n"
"  t.dataset.sortDir = dir === 1 ? 'asc' : 'desc';\n"
"  rows.sort(function(a,b) {\n"
"    var va = a.cells[col].dataset.val || a.cells[col].textContent;\n"
"    var vb = b.cells[col].dataset.val || b.cells[col].textContent;\n"
"    var na = parseFloat(va), nb = parseFloat(vb);\n"
"    if (!isNaN(na) && !isNaN(nb)) return (na - nb) * dir;\n"
"    return va.localeCompare(vb) * dir;\n"
"  });\n"
"  rows.forEach(function(r) { t.tBodies[0].appendChild(r); });\n"
"}\n"
"</script>\n"
"</head>\n"
"<body>\n"
"<div class=\"container\">\n";

static const char *report_html_footer =
"</div>\n"
"<div class=\"footer\">\n"
"  Generated by pg_rat — PostgreSQL Real Application Testing Extension\n"
"</div>\n"
"</body>\n"
"</html>\n";

PG_FUNCTION_INFO_V1(pg_rat_generate_report);

/*
 * pg_rat_generate_report(filepath text) → text
 *
 * Reads pg_rat_spa_results via SPI and generates a styled HTML
 * regression report. Writes to filepath if provided, otherwise
 * returns the HTML as text.
 */
Datum
pg_rat_generate_report(PG_FUNCTION_ARGS)
{
	text	   *filepath_text = PG_ARGISNULL(0) ? NULL : PG_GETARG_TEXT_PP(0);
	const char *filepath = filepath_text ? text_to_cstring(filepath_text) : NULL;
	StringInfoData html;
	int			ret;
	uint64		total_queries = 0;
	uint64		regressions = 0;
	uint64		improvements = 0;
	uint64		unchanged = 0;

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("pg_rat: SPI_connect failed")));

	/* Fetch all results ordered by regression severity */
	ret = SPI_execute("SELECT query_hash, query_text, "
					  "source_elapsed_ms, target_elapsed_ms, "
					  "source_buffers, target_buffers, "
					  "regression_pct, plan_changed, exec_count "
					  "FROM pg_rat_spa_results "
					  "ORDER BY regression_pct DESC",
					  true, 0);

	if (ret != SPI_OK_SELECT)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("pg_rat: could not query pg_rat_spa_results")));

	initStringInfo(&html);

	/* Write HTML header */
	appendStringInfoString(&html, report_html_header);

	/* Header section */
	appendStringInfoString(&html,
						   "<div class=\"header\">\n"
						   "  <h1>&#x1f50d; SQL Performance Analysis Report</h1>\n"
						   "  <p>pg_rat — PostgreSQL Real Application Testing</p>\n"
						   "</div>\n");

	/* Count summary stats */
	total_queries = SPI_processed;
	{
		uint64		i;

		for (i = 0; i < SPI_processed; i++)
		{
			char	   *reg_str = SPI_getvalue(SPI_tuptable->vals[i],
											   SPI_tuptable->tupdesc, 7);
			double		reg_pct = reg_str ? atof(reg_str) : 0.0;

			if (reg_pct > 10.0)
				regressions++;
			else if (reg_pct < -10.0)
				improvements++;
			else
				unchanged++;
		}
	}

	/* Stats cards */
	appendStringInfo(&html,
					 "<div class=\"stats-grid\">\n"
					 "  <div class=\"stat-card\">\n"
					 "    <div class=\"label\">Total Queries</div>\n"
					 "    <div class=\"value blue\">%llu</div>\n"
					 "  </div>\n"
					 "  <div class=\"stat-card\">\n"
					 "    <div class=\"label\">Regressions (&gt;10%%)</div>\n"
					 "    <div class=\"value red\">%llu</div>\n"
					 "  </div>\n"
					 "  <div class=\"stat-card\">\n"
					 "    <div class=\"label\">Improvements (&gt;10%%)</div>\n"
					 "    <div class=\"value green\">%llu</div>\n"
					 "  </div>\n"
					 "  <div class=\"stat-card\">\n"
					 "    <div class=\"label\">Unchanged</div>\n"
					 "    <div class=\"value yellow\">%llu</div>\n"
					 "  </div>\n"
					 "</div>\n",
					 (unsigned long long) total_queries, (unsigned long long) regressions, (unsigned long long) improvements, (unsigned long long) unchanged);

	/* Results table */
	appendStringInfoString(&html,
						   "<table id=\"results\">\n"
						   "<thead>\n"
						   "<tr>\n"
						   "  <th onclick=\"sortTable(0)\">#</th>\n"
						   "  <th onclick=\"sortTable(1)\">Query</th>\n"
						   "  <th onclick=\"sortTable(2)\">Source (ms)</th>\n"
						   "  <th onclick=\"sortTable(3)\">Target (ms)</th>\n"
						   "  <th onclick=\"sortTable(4)\">Regression %</th>\n"
						   "  <th onclick=\"sortTable(5)\">Source Buffers</th>\n"
						   "  <th onclick=\"sortTable(6)\">Target Buffers</th>\n"
						   "  <th onclick=\"sortTable(7)\">Executions</th>\n"
						   "  <th onclick=\"sortTable(8)\">Plan Changed</th>\n"
						   "</tr>\n"
						   "</thead>\n"
						   "<tbody>\n");

	/* Write rows */
	{
		uint64		i;

		for (i = 0; i < SPI_processed; i++)
		{
			char	   *query_hash = SPI_getvalue(SPI_tuptable->vals[i],
												  SPI_tuptable->tupdesc, 1);
			char	   *query_text = SPI_getvalue(SPI_tuptable->vals[i],
												  SPI_tuptable->tupdesc, 2);
			char	   *source_ms = SPI_getvalue(SPI_tuptable->vals[i],
												 SPI_tuptable->tupdesc, 3);
			char	   *target_ms = SPI_getvalue(SPI_tuptable->vals[i],
												 SPI_tuptable->tupdesc, 4);
			char	   *source_buf = SPI_getvalue(SPI_tuptable->vals[i],
												  SPI_tuptable->tupdesc, 5);
			char	   *target_buf = SPI_getvalue(SPI_tuptable->vals[i],
												  SPI_tuptable->tupdesc, 6);
			char	   *reg_pct = SPI_getvalue(SPI_tuptable->vals[i],
											   SPI_tuptable->tupdesc, 7);
			char	   *plan_changed = SPI_getvalue(SPI_tuptable->vals[i],
													SPI_tuptable->tupdesc, 8);
			char	   *exec_count = SPI_getvalue(SPI_tuptable->vals[i],
												  SPI_tuptable->tupdesc, 9);
			double		pct = reg_pct ? atof(reg_pct) : 0.0;
			const char *css_class;
			char		truncated_query[128];
			int			qlen;

			if (pct > 10.0)
				css_class = "regression";
			else if (pct < -10.0)
				css_class = "improvement";
			else
				css_class = "neutral";

			/* Truncate query for display */
			qlen = query_text ? strlen(query_text) : 0;
			if (qlen > 120)
			{
				memcpy(truncated_query, query_text, 117);
				truncated_query[117] = '.';
				truncated_query[118] = '.';
				truncated_query[119] = '.';
				truncated_query[120] = '\0';
			}
			else if (query_text)
			{
				strlcpy(truncated_query, query_text, sizeof(truncated_query));
			}
			else
			{
				truncated_query[0] = '\0';
			}

			appendStringInfo(&html,
							 "<tr>\n"
							 "  <td>%llu</td>\n"
							 "  <td class=\"query-text\" title=\"%s\">%s</td>\n"
							 "  <td data-val=\"%s\">%s</td>\n"
							 "  <td data-val=\"%s\">%s</td>\n"
							 "  <td data-val=\"%s\" class=\"%s\">%s%%</td>\n"
							 "  <td data-val=\"%s\">%s</td>\n"
							 "  <td data-val=\"%s\">%s</td>\n"
							 "  <td data-val=\"%s\">%s</td>\n"
							 "  <td>%s</td>\n"
							 "</tr>\n",
							 (unsigned long long) (i + 1),
							 query_text ? query_text : "",
							 truncated_query,
							 source_ms ? source_ms : "0",
							 source_ms ? source_ms : "0",
							 target_ms ? target_ms : "0",
							 target_ms ? target_ms : "0",
							 reg_pct ? reg_pct : "0",
							 css_class,
							 reg_pct ? reg_pct : "0",
							 source_buf ? source_buf : "0",
							 source_buf ? source_buf : "0",
							 target_buf ? target_buf : "0",
							 target_buf ? target_buf : "0",
							 exec_count ? exec_count : "0",
							 exec_count ? exec_count : "0",
							 plan_changed ? plan_changed : "false");
		}
	}

	appendStringInfoString(&html,
						   "</tbody>\n"
						   "</table>\n");

	/* Footer */
	appendStringInfoString(&html, report_html_footer);

	SPI_finish();

	/* Write to file or return as text */
	if (filepath)
	{
		FILE	   *fp = fopen(filepath, "w");

		if (fp == NULL)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", filepath)));

		fwrite(html.data, 1, html.len, fp);
		fclose(fp);

		elog(LOG, "pg_rat: report written to \"%s\" (%d bytes)", filepath, html.len);

		PG_RETURN_TEXT_P(cstring_to_text(filepath));
	}
	else
	{
		PG_RETURN_TEXT_P(cstring_to_text(html.data));
	}
}
