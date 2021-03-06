#include "gc.h"

/*
 * Tokenization code based on wannabe-regex.c,
 * which I wrote some months ago
 */

/* TODO: cleanup some of the more mysterious
 *       parts of the code...
 */

#include "tokenizer.h"
#include "tokens.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *code_lines[1024];
extern void fail(char* mesg);
extern void sanity_requires(int exp);

typedef struct match_struct {
	/* accept number, if any */
	int token;
	
	/* pointer to last character matched */
	char* pos;
} match_t;

typedef struct trie_struct {
	/* character edges */
	struct trie_struct* map[256];

	/* epsilon edges */
	struct trie_struct** link;
	int links;
	int link_alloc;

	/* accept state, if any */
	int valid_token;
} trie;

/* ============================================ */

/* Trie routines */

void add_link(trie* from, trie* to)
{
	if (++from->links > from->link_alloc)
		from->link = realloc(from->link, 
			(from->link_alloc += 5) * sizeof(trie *));

	if (!from->link)
		fail("link array allocation");

	from->link[from->links - 1] = to;
}

trie* new_trie()
{
	trie* t = cgc_malloc(sizeof(trie));
	int i;
	if (!t)
		fail("trie allocation");
	t->link = cgc_malloc(10 * sizeof(trie*));
	if (!t->link)
		fail("trie links");
	t->links = 0;
	t->link_alloc = 10;
	for (i = 0; i < 10; i++)
		t->link[i] = NULL;
	t->valid_token = 0;
	for(i = 0; i < 256; i++)
		t->map[i] = NULL;
	return t;
}

/* ============================================ */

/* Pattern compiler routine */

void add_token(trie* t, char* tok, int key)
{
	int i, j;
	int c;
	trie *hist[1024];
	trie *next;
	int len = strlen(tok);
	char* new_tok;
	int n;

	new_tok = cgc_malloc(len + 32);
	if (!new_tok)
		fail("buffer allocation");
	strcpy(new_tok, tok);

	for (i = 0, n = 0; c = new_tok[i], i < len && t; ++i)
	{
		hist[n] = t;

		switch (c) {
			case 'A':	/* special: set of all letters */
				next = new_trie();
				for(j = 'a'; j <= 'z'; j++)
					t->map[j] = next;
				for(j = 'A'; j <= 'Z'; j++)
					t->map[j] = next;
				t->map['_'] = next;
				t = next;
				++n;
				break;
			case 'B':	/* special: set of all letters 
					   + all digits */
				next = new_trie();
				for(j = 'a'; j <= 'z'; j++)
					t->map[j] = next;
				for(j = 'A'; j <= 'Z'; j++)
					t->map[j] = next;
				for(j = '0'; j <= '9'; j++)
					t->map[j] = next;
				t->map['_'] = next;
				t = next;
				++n;
				break;
			case 'D':	/* special: set of all digits */
				next = new_trie();
				for(j = '0'; j <= '9'; j++)
					t->map[j] = next;
				t = next;
				++n;
				break;
			case 'W':	/* special: whitespace */
				next = new_trie();
				t->map[' '] = next;
				t->map['\t'] = next;
				t = next;
				++n;
				break;
			case '?':	/* optional character */
				sanity_requires(n > 0);

				/* 
				 * The previous node is allowed to
				 * go straight to this node, via
				 * an epsilon link.
				 */
				add_link(hist[n - 1], t);
				break;
			case '+':	/* character can repeat
					   any number of times */
				sanity_requires(n > 0);

				/* 
				 * Make a "buffer" node to keep things
				 * cleanly separated and avoid
				 * surprises when matching e.g.
				 * 'a+b+'. Epsilon-link to it.
				 */
				add_link(t, hist[++n] = new_trie());
				t = hist[n];

				/* 
				 * Next, we epsilon-link the buffer node
				 * all the way back to the node '+' compilation
				 * started with, to allow repetition.
				 */
				add_link(t, hist[n - 2]);

				/* 
				 * And finally we make a clean
				 * new node for further work,
				 * and epsilon-link to it.
				 */
				add_link(t, hist[++n] = new_trie());
				t = hist[n];
				break;
			case '*':	/* optional character
					   with repetition allowed */
				sanity_requires(n > 0);

				/* same as +, except that the
 				 * second part makes a mutual link */

				add_link(t, hist[++n] = new_trie());
				t = hist[n];
				
				/* mutual link */
				add_link(t, hist[n - 2]);
				add_link(hist[n - 2], t);

				add_link(t, hist[++n] = new_trie());
				t = hist[n];
				break;
			case '\\':	/* escape to normal text */
				if (++i == len)
					fail("backslash expected char");
				c = new_tok[i];
			default:	/* normal text */
				if (!t->map[c])
					t->map[c] = new_trie();
				t = t->map[c];
				++n;
		}
	}

	/* 
	 * We have reached the accept-state node;
	 * mark it as such.
	 */
	t->valid_token = key;
}


/*
 * NFA trie evaluator (uses backtracking) 
 * Has implementation-time hacks, parameters and all
 * (aka bloat)
 */

match_t match(trie* automaton, char* full_text, char* where)
{
	extern match_t match_algo(trie* t, char* full_tok, char* tok,
		int frk, trie* led, int ledi);

	return match_algo(automaton, full_text, where, 0, NULL, 0);
}

/* trie* t		:	matching automaton / current node
 * char* full_tok	:	full text
 * char* tok		: 	where we are now
 * int frk		: 	internal. initially, give 0
 * trie* led		:	internal. start with NULL
 * int ledi		:	internal. start with 0
 */	
match_t match_algo(trie* t, char* full_tok, char* tok, int frk, 
	trie* led, int ledi)
{
	int i = 0;
	int j;
	char c;

	/* greedy choice registers */
	int record;		/* longest match */
	match_t m;		/* match register */
	match_t ml[10];		/* match list */
	int mc = 0;		/* match count */
	match_t* choice;	/* final choice */	

	int len = strlen(tok) + 1;

	int last_inc;

	i = 0; do
	{
		last_inc = 0;
		c = tok[i];

		/* check if multiple epsilon edges exist
		 * and if we are not already in a fork */ 
		if (t->links && !frk)
		{
			/* fork for each possibility
			 * and collect results in an array */

			/* character edge: frk = 1 */
			if (t->map[c])
				if ((m = match_algo(t, full_tok, tok + i, 1,
					led, ledi)).token)
					ml[mc++] = m;

			/* epsilon edge(s): frk = 2 + link number */
			for (j = 0; j < t->links; j++)
				if ((m = match_algo(t, full_tok, tok + i, 
					j + 2, led, ledi)).token)
					ml[mc++] = m;

			/* if there are multiple epsilon matches,
			 * be "greedy", i.e. choose longest match. */
			if (mc > 1) {
				record = 0;
				choice = NULL;
				for (j = 0; j < mc; j++)
					if (ml[j].pos - full_tok >= record) {
						choice = &ml[j];
						record = ml[j].pos - full_tok;
					}
				if (!choice) fail("greedy choice");
				return *choice;
			}
			else if (mc == 1) /* only one match */
				return *ml;
			else	/* no match, return blank as-is */
				return m;
		}

		/* if we are not in an epsilon fork
		 * and the character edge exists,
		 * follow it. */
		if (frk <= 1 && t->map[c]) {
			t = t->map[c];

			last_inc = 1;

			/* break loop if in matching state */
			if (t->valid_token)
				break;
			
			++i;
			goto valid;
		}
		else if (frk > 1) {
			/* prevents infinite loops: if the
			 * link chosen in this fork brings us back
			 * to the last link followed and the character
			 * index has not increased since we followed 
			 * this last link, do not follow it */
			if (!(led && t->link[frk - 2] == led
			    && (int)(&tok[i] - full_tok) == ledi)) {

				/* t->links == 1 is special (???) */
				if (t->links > 1) {
					led = t; /* Last Epsilon departure
						    node */
					ledi = (int)(&tok[i] - full_tok);
				}

				t = t->link[frk - 2];
					
				goto valid;
			}
		}

		break;

		/* clear fork path number after the first
		 * iteration of the loop, it is not relevant
		 * afterwards */
valid:		if (frk)
			frk = 0;

	} while (i < len && t);

	/* set up the returned structure */
	if (t && t->valid_token) {
		/* match */
		m.token = t->valid_token;
		m.pos = tok + i + last_inc;
	} else {
		/* no match */
		m.token = 0;
	}

	return m;
}

trie *t[100];
int tc = 0;

token_t* tokenize(char *in_buf)
{
	match_t m;
	match_t c;
	char buf2[1024];
	int max;
	int i;
	int line = 1;
	char *line_start;
	token_t *toks = cgc_malloc(64 * sizeof(token_t));
	int tok_alloc = 64;
	int tok_count = 0;
	char *buf = cgc_malloc(strlen(in_buf) + 1);
	char *p;

	strcpy(buf, in_buf);
	buf[strlen(in_buf)] = 0;

	*code_lines = buf;
	line_start = buf;


	for(p = buf; *p; ) {
		max = -1;

		/* 
		 * Choose the matching pattern
		 * that gives the longest match.
		 * Shit is "meta-greedy".
		 */
		for (i = 0; i < tc; i++) {
			m = match(t[i], buf, p);
			if (m.token) {	/* match has succeeded */
				if (m.pos - p > max) {
					c = m;
					max = m.pos - p;
				}
			}
		}

		if (max == -1) {
			/* matching from this offset failed */
			fail("tokenization failed");
		} else {
			/* add token to the tokens array */
			if (c.token != TOK_WHITESPACE
			 && c.token != TOK_NEWLINE) {
				if (++tok_count > tok_alloc) {
					tok_alloc = tok_count + 64;
					toks = realloc(toks,
						tok_alloc * sizeof(token_t));
					if (!toks)
						fail("resize tokens array");
				}
				toks[tok_count - 1].type = c.token;
				toks[tok_count - 1].start = p;
				toks[tok_count - 1].len = max;
				toks[tok_count - 1].from_line = line;
				toks[tok_count - 1].from_char = p - 
					line_start + 1;
			}

advance:
			/* move forward in the string */
			p += max;
			if (max == 0)
				++p;

			/* line accounting (useful for
		 	 * pretty parse-fail diagnostics) */
			if (c.token == TOK_NEWLINE) {
				code_lines[line] = line_start;
				++line;
				line_start = p;
			}
		}
	}

	/* the final token is a bit special */
	toks[tok_count].start = NULL;
	toks[tok_count].len = 0;
	toks[tok_count].from_line = line;
	toks[tok_count].from_char = p - line_start;
	code_lines[line] = line_start;

	return toks;
}

void setup_tokenizer()
{
	int i = 0;

	/* set up the automatons that
	 * match the various token types */

	for (i = 0; i < 100; i++)
		t[i] = new_trie();

	/* D: any digit */
	add_token(t[tc++], "D+", TOK_INTEGER);

	/* W: any whitespace character */
	add_token(t[tc++], "W+", TOK_WHITESPACE);

	/* A: letter; B: letter or digit */
	add_token(t[tc++], "AB*", TOK_IDENT);

	/* operators */
	add_token(t[tc++], "\\+", TOK_PLUS);
	add_token(t[tc++], "-", TOK_MINUS);
	add_token(t[tc++], "/", TOK_DIV);
	add_token(t[tc++], "\\*", TOK_MUL);
	add_token(t[tc++], "=", TOK_ASGN);

	/* special characters */

	add_token(t[tc++], "(", TOK_LPAREN);
	add_token(t[tc++], ")", TOK_RPAREN);
	add_token(t[tc++], ";", TOK_SEMICOLON);
	add_token(t[tc++], ",", TOK_COMMA);
	add_token(t[tc++], "\n", TOK_NEWLINE);
	add_token(t[tc++], ":", TOK_COLON);
	add_token(t[tc++], "[", TOK_LBRACK);
	add_token(t[tc++], "]", TOK_RBRACK);

	add_token(t[tc++], "'", TOK_QUOT);
	add_token(t[tc++], "@", TOK_NAN);

	add_token(t[tc++], "#", TOK_MNUM);
	add_token(t[tc++], "\\?", TOK_MVAR);
	add_token(t[tc++], "|", TOK_PIPE);
	add_token(t[tc++], "\\$", TOK_DOLLAR);

	add_token(t[tc++], "\\^", TOK_EXP);

	add_token(t[tc++], "::", TOK_CC);

	add_token(t[tc++], "|'", TOK_NOTSYMBOL);
}

