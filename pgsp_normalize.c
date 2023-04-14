/*-------------------------------------------------------------------------
 *
 * pgsp_json.c: Plan handler for JSON/XML/YAML style plans
 *
 * Copyright (c) 2012-2022, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 * IDENTIFICATION
 *	  pg_store_plans/pgsp_json.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"

#include "nodes/parsenodes.h"

#include "parser/scanner.h"
#include "parser/gram.h"

#define INDENT_STEP 2


void normalize_expr(char *expr, bool preserve_space);

/*
 * Look for these operator characters in order to decide whether to strip
 * whitespaces which are needless from the view of sql syntax in
 * normalize_expr(). This must be synced with op_chars in scan.l.
 */
#define OPCHARS "~!@#^&|`?+-*/%<>="
#define IS_WSCHAR(c) ((c) == ' ' || (c) == '\n' || (c) == '\t')
#define IS_CONST(tok) (tok == FCONST || tok == SCONST || tok == BCONST || \
			tok == XCONST || tok == ICONST || tok == NULL_P || \
		    tok == TRUE_P || tok == FALSE_P || \
			tok == CURRENT_DATE || tok == CURRENT_TIME || \
		    tok == LOCALTIME || tok == LOCALTIMESTAMP)
#define IS_INDENTED_ARRAY(v) ((v) == P_GroupKeys || (v) == P_HashKeys)

/*
 * norm_yylex: core_yylex with replacing some tokens.
 */
static int
norm_yylex(char *str, core_YYSTYPE *yylval, YYLTYPE *yylloc, core_yyscan_t yyscanner)
{
	int tok;

	PG_TRY();
	{
		tok = core_yylex(yylval, yylloc, yyscanner);
	}
	PG_CATCH();
	{
		/*
		 * Error might occur during parsing quoted tokens that chopped
		 * halfway. Just ignore the rest of this query even if there might
		 * be other reasons for parsing to fail.
		 */
		FlushErrorState();
		return -1;
	}
	PG_END_TRY();

	/*
	 * '?' alone is assumed to be an IDENT.  If there's a real
	 * operator '?', this should be confused but there's hardly be.
	 */
	if (tok == Op && str[*yylloc] == '?' &&
		strchr(OPCHARS, str[*yylloc + 1]) == NULL)
		tok = SCONST;

	/*
	 * Replace tokens with '=' if the operator is consists of two or
	 * more opchars only. Assuming that opchars do not compose a token
	 * with non-opchars, check the first char only is sufficient.
	 */
	if (tok == Op && strchr(OPCHARS, str[*yylloc]) != NULL)
		tok = '=';

	return tok;
}

/*
 * normalize_expr - Normalize statements or expressions.
 *
 * Mask constants, strip unnecessary whitespaces and upcase keywords. expr is
 * modified in-place (destructively). If readability is more important than
 * uniqueness, preserve_space puts one space for one existent whitespace for
 * more readability.
 */
/* scanner interface is changed in PG12 */
#if PG_VERSION_NUM < 120000
#define ScanKeywords (*ScanKeywords)
#define ScanKeywordTokens NumScanKeywords
#endif
void
normalize_expr(char *expr, bool preserve_space)
{
	core_yyscan_t yyscanner;
	core_yy_extra_type yyextra;
	core_YYSTYPE yylval;
	YYLTYPE		yylloc;
	YYLTYPE		lastloc;
	YYLTYPE start;
	char *wp;
	int			tok, lasttok;

	wp = expr;
	yyscanner = scanner_init(expr,
							 &yyextra,
							 &ScanKeywords,
							 ScanKeywordTokens);

	/*
	 * The warnings about nonstandard escape strings is already emitted in the
	 * core. Just silence them here.
	 */
#if PG_VERSION_NUM >= 90500
	yyextra.escape_string_warning = false;
#endif
	lasttok = 0;
	lastloc = -1;

	for (;;)
	{
		tok = norm_yylex(expr, &yylval, &yylloc, yyscanner);

		start = yylloc;

		if (lastloc >= 0)
		{
			int i, i2;

			/* Skipping preceding whitespaces */
			for(i = lastloc ; i < start && IS_WSCHAR(expr[i]) ; i++);

			/* Searching for trailing whitespace */
			for(i2 = i; i2 < start && !IS_WSCHAR(expr[i2]) ; i2++);

			if (lasttok == IDENT)
			{
				/* Identifiers are copied in case-sensitive manner. */
				memcpy(wp, expr + i, i2 - i);
				wp += i2 - i;
			}
#if PG_VERSION_NUM >= 100000
			/*
			 * Since PG10 pg_stat_statements doesn't store trailing semicolon
			 * in the column "query". Normalization is basically useless in the
			 * version but still usefull to match utility commands so follow
			 * the behavior change.
			 */
			else if (lasttok == ';')
			{
				/* Just do nothing */
			}
#endif
			else
			{
				/* Upcase keywords */
				char *sp;
				for (sp = expr + i ; sp < expr + i2 ; sp++, wp++)
					*wp = (*sp >= 'a' && *sp <= 'z' ?
						   *sp - ('a' - 'A') : *sp);
			}

			/*
			 * Because of destructive writing, wp must not go advance the
			 * reading point.
			 * Although this function's output does not need any validity as a
			 * statement or an expression, spaces are added where it should be
			 * to keep some extent of sanity.  If readability is more important
			 * than uniqueness, preserve_space adds one space for each
			 * existent whitespace.
			 */
			if (tok > 0 &&
				i2 < start &&
				(preserve_space ||
				 (tok >= IDENT && lasttok >= IDENT &&
				  !IS_CONST(tok) && !IS_CONST(lasttok))))
				*wp++ = ' ';

			start = i2;
		}

		/* Exit on parse error. */
		if (tok < 0)
		{
			*wp = 0;
			return;
		}

		/*
		 * Negative signs before numbers are tokenized separately. And
		 * explicit positive signs won't appear in deparsed expressions.
		 */
		if (tok == '-')
			tok = norm_yylex(expr, &yylval, &yylloc, yyscanner);

		/* Exit on parse error. */
		if (tok < 0)
		{
			*wp = 0;
			return;
		}

		if (IS_CONST(tok))
		{
			YYLTYPE end;

			tok = norm_yylex(expr, &yylval, &end, yyscanner);

			/* Exit on parse error. */
			if (tok < 0)
			{
				*wp = 0;
				return;
			}

			/*
			 * Negative values may be surrounded with parens by the
			 * deparser. Mask involving them.
			 */
			if (lasttok == '(' && tok == ')')
			{
				wp -= (start - lastloc);
				start = lastloc;
				end++;
			}

			while (expr[end - 1] == ' ')
				end--;

			*wp++ = '?';
			yylloc = end;
		}

		if (tok == 0)
			break;

		lasttok = tok;
		lastloc = yylloc;
	}
	*wp = 0;
}
