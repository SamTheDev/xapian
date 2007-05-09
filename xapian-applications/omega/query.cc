/* query.cc: query executor for omega
 *
 * Copyright 1999,2000,2001 BrightStation PLC
 * Copyright 2001 James Aylett
 * Copyright 2001,2002 Ananova Ltd
 * Copyright 2002 Intercede 1749 Ltd
 * Copyright 2002,2003,2004,2005,2006,2007 Olly Betts
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301
 * USA
 */

#include <config.h>

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <vector>

#include <assert.h>
#include <ctype.h>
#include "safeerrno.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "strcasecmp.h"
#include <time.h>

#include "safeunistd.h"
#include <sys/types.h>
#include "safesysstat.h"
#include "safefcntl.h"

#ifdef HAVE_GETTIMEOFDAY
#include <sys/time.h>
#elif defined HAVE_FTIME
#include <sys/timeb.h>
#endif

#include <cdb.h>

#ifdef HAVE_PCRE
#include <pcre.h>
#endif

#include "date.h"
#include "datematchdecider.h"
#include "utils.h"
#include "omega.h"
#include "query.h"
#include "cgiparam.h"
#include "loadfile.h"
#include "values.h"

#include <xapian.h>

using namespace std;

using Xapian::Utf8Iterator;

using Xapian::Unicode::is_wordchar;

#ifndef SNPRINTF
#include <stdarg.h>

static int my_snprintf(char *str, size_t size, const char *format, ...)
{
    int res;
    va_list ap;
    va_start(ap, format);
    str[size - 1] = '\0';
    res = vsprintf(str, format, ap);
    if (str[size - 1] || res < 0 || size_t(res) >= size)
	abort(); /* Overflowed! */
    va_end(ap);
    return res;
}
#else
#define my_snprintf SNPRINTF
#endif

static bool query_parsed = false;
static bool done_query = false;
static Xapian::docid last = 0;

static Xapian::MSet mset;

static map<Xapian::docid, bool> ticked;

static void ensure_query_parsed();
static void ensure_match();

static Xapian::Query query;
//static string url_query_string;
Xapian::Query::op default_op = Xapian::Query::OP_OR; // default matching mode

static Xapian::QueryParser qp;
static Xapian::Stem *stemmer = NULL;

static string eval_file(const string &fmtfile);

static set<string> termset;

static string queryterms;

static string error_msg;

static long int sec = 0, usec = -1;

static const char DEFAULT_LOG_ENTRY[] =
	"$or{$env{REMOTE_HOST},$env{REMOTE_ADDR},-}\t"
	"[$date{$now,%d/%b/%Y:%H:%M:%S} +0000]\t"
	"$if{$cgi{X},add,$if{$cgi{MORELIKE},morelike,query}}\t"
	"$dbname\t"
	"$query\t"
	"$msize$if{$env{HTTP_REFERER},\t$env{HTTP_REFERER}}";

static bool set_content_type = false;

class MyStopper : public Xapian::Stopper {
  public:
    bool operator()(const string &t) const {
	switch (t[0]) {
	    case 'a':
		return (t == "a" || t == "about" || t == "an" || t == "and" ||
			t == "are" || t == "as" || t == "at");
	    case 'b':
		return (t == "be" || t == "by");
	    case 'e':
		return (t == "en");
	    case 'f':
		return (t == "for" || t == "from");
	    case 'h':
		return (t == "how");
	    case 'i':
		return (t == "i" || t == "in" || t == "is" || t == "it");
	    case 'o':
		return (t == "of" || t == "on" || t == "or");
	    case 't':
		return (t == "that" || t == "the" || t == "this" || t == "to");
	    case 'w':
		return (t == "was" || t == "what" || t == "when" ||
			t == "where" || t == "which" || t == "who" ||
			t == "why" || t == "will" || t == "with");
	    default:
		return false;
	}
    }
};

// Don't allow ".." in format names, log file names, etc as this would allow
// people to open a format "../../etc/passwd" or similar.
// FIXME: make this check more exact ("foo..bar" is safe)
// FIXME: log when this check fails
static bool
vet_filename(const string &filename)
{
    string::size_type i = filename.find("..");
    return (i == string::npos);
}

// Heuristics:
// * If any terms have been removed, it's a "fresh query" so we discard any
//   relevance judgements
// * If all previous terms are there but more have been added then we keep
//   the relevance judgements, but return the first page of hits
//
// NEW_QUERY entirely new query
// SAME_QUERY unchanged query
// EXTENDED_QUERY new query, but based on the old one
// BAD_QUERY parse error (message in error_msg)
typedef enum { NEW_QUERY, SAME_QUERY, EXTENDED_QUERY, BAD_QUERY } querytype;

static querytype
set_probabilistic(const string &oldp)
{
    // Parse the query string.
    qp.set_stemmer(Xapian::Stem(option["stemmer"]));
    qp.set_stemming_strategy(option["stem_all"] == "true" ? Xapian::QueryParser::STEM_ALL : Xapian::QueryParser::STEM_SOME);
    qp.set_stopper(new MyStopper());
    qp.set_default_op(default_op);
    qp.set_database(db);
    map<string, string>::const_iterator pfx = option.lower_bound("prefix,");
    for (; pfx != option.end() && pfx->first.substr(0, 7) == "prefix,"; ++pfx) {
	qp.add_prefix(pfx->first.substr(7), pfx->second);
    }
    pfx = option.lower_bound("boolprefix,");
    for (; pfx != option.end() && pfx->first.substr(0, 11) == "boolprefix,"; ++pfx) {
	qp.add_boolean_prefix(pfx->first.substr(11), pfx->second);
    }

    try {
	query = qp.parse_query(query_string);
    } catch (const char *s) {
	error_msg = s;
	return BAD_QUERY;
    }

    Xapian::termcount n_new_terms = 0;
    for (Xapian::TermIterator i = query.get_terms_begin();
	 i != query.get_terms_end(); ++i) {
	if (termset.find(*i) == termset.end()) {
	    termset.insert(*i);
	    if (!queryterms.empty()) queryterms += '\t';
	    queryterms += *i;
	}
	n_new_terms++;
    }

    // Check new query against the previous one
    if (oldp.empty()) return query_string.empty() ? SAME_QUERY : NEW_QUERY;

    // Long, long ago we used "word1#word2#" (with trailing #) but some broken
    // old browsers (versions of MSIE) don't quote # in form GET submissions
    // and everything after the # gets interpreted as an anchor.  We now allow
    // terms like `c#' so we want to avoid '#' anyway.
    //
    // So we switched to using "word1.word2." but that doesn't work if
    // the terms contain "." themselves (e.g. Tapplication/vnd.ms-excel)
    // so now we use "word1\tword2" instead (with no trailing separator).
    //
    // However for compatibility with templates which haven't been updated and
    // bookmarked queries from Omega 0.9.6 and earlier we still support ".".
    char separator = '\t';
    unsigned int n_old_terms = count(oldp.begin(), oldp.end(), '\t') + 1;
    if (n_old_terms == 1 && oldp[oldp.size() - 1] == '.') {
	separator = '.';
	n_old_terms = count(oldp.begin(), oldp.end(), '.');
    }

    // short-cut: if the new query has fewer terms, it must be a new one
    if (n_new_terms < n_old_terms) return NEW_QUERY;

    const char *term = oldp.c_str();
    const char *pend;
    while ((pend = strchr(term, separator)) != NULL) {
	if (termset.find(string(term, pend - term)) == termset.end())
	    return NEW_QUERY;
	term = pend + 1;
    }
    if (*term) {
	if (termset.find(string(term)) == termset.end())
	    return NEW_QUERY;
    }

    // Use termset.size() rather than n_new_terms so we correctly handle
    // the case when the query has repeated terms.
    // This works wrongly in the case when the user extends the query
    // by adding a term already in it, but that's unlikely and the behaviour
    // isn't too bad (we just don't reset page 1).  We also mishandle a few
    // other obscure cases e.g. adding quotes to turn a query into a phrase.
    if (termset.size() > n_old_terms) return EXTENDED_QUERY;
    return SAME_QUERY;
}

static multimap<string, string> filter_map;

typedef multimap<string, string>::const_iterator FMCI;

void add_bterm(const string &term) {
    string prefix;
    if (term[0] == 'X') {
	string::const_iterator i = term.begin() + 1;
	while (i != term.end() && isupper(static_cast<unsigned char>(*i))) ++i;
	if (i != term.end() && *i == ':') ++i;
	prefix = string(term.begin(), i);
    } else {
	prefix = term[0];
    }

    filter_map.insert(multimap<string, string>::value_type(prefix, term));
}

static void
run_query()
{
    if (!filter_map.empty()) {
	// OR together filters with the same prefix, then AND together
	vector<Xapian::Query> filter_vec;
	vector<string> or_vec;
	string current;
	for (FMCI i = filter_map.begin(); ; i++) {
	    bool over = (i == filter_map.end());
	    if (over || i->first != current) {
		switch (or_vec.size()) {
		    case 0:
		        break;
		    case 1:
			filter_vec.push_back(Xapian::Query(or_vec[0]));
		        break;
		    default:
			filter_vec.push_back(Xapian::Query(Xapian::Query::OP_OR,
						     or_vec.begin(),
						     or_vec.end()));
		        break;
		}
		or_vec.clear();
		if (over) break;
		current = i->first;
	    }
	    or_vec.push_back(i->second);
	}

	Xapian::Query filter(Xapian::Query::OP_AND,
			     filter_vec.begin(), filter_vec.end());

	if (query.empty()) {
	    // If no probabilistic query is provided then promote the filters
	    // to be THE query - filtering an empty query will give no
	    // matches.
	    std::swap(query, filter);
	    if (enquire) enquire->set_weighting_scheme(Xapian::BoolWeight());
	} else {
	    query = Xapian::Query(Xapian::Query::OP_FILTER, query, filter);
	}
    }

    Xapian::MatchDecider * mdecider = NULL;
    if (!date_start.empty() || !date_end.empty() || !date_span.empty()) {
	MCI i = cgi_params.find("DATEVALUE");
	if (i != cgi_params.end()) {
	    Xapian::valueno datevalue = string_to_int(i->second);
	    mdecider = new DateMatchDecider(datevalue, date_start, date_end, date_span);
	} else {
	    Xapian::Query date_filter(Xapian::Query::OP_OR,
				      date_range_filter(date_start, date_end,
							date_span),
				      Xapian::Query("Dlatest"));

	    // If no probabilistic query is provided then promote the daterange
	    // filter to be THE query instead of filtering an empty query.
	    if (query.empty()) {
		query = date_filter;
	    } else {
		query = Xapian::Query(Xapian::Query::OP_FILTER, query, date_filter);
	    }
	}
    }

    if (!enquire || !error_msg.empty()) return;

    enquire->set_cutoff(threshold);

    if (sort_key != Xapian::valueno(-1)) {
	if (sort_after) {
	    enquire->set_sort_by_relevance_then_value(sort_key, sort_ascending);
	} else {
	    enquire->set_sort_by_value_then_relevance(sort_key, sort_ascending);
	}
    }

    enquire->set_docid_order(docid_order);

    if (collapse) {
	enquire->set_collapse_key(collapse_key);
    }

#ifdef HAVE_GETTIMEOFDAY
    struct timeval tv;
    if (gettimeofday(&tv, 0) == 0) {
	sec = tv.tv_sec;
	usec = tv.tv_usec;
    }
#elif defined(FTIME_RETURNS_VOID)
    struct timeb tp;
    ftime(&tp);
    sec = tp.time;
    usec = tp.millitm * 1000;
#elif defined(HAVE_FTIME)
    struct timeb tp;
    if (ftime(&tp) == 0) {
	sec = tp.time;
	usec = tp.millitm * 1000;
    }
#else
    sec = time(NULL);
    if (sec != (time_t)-1) usec = 0;
#endif
    if (!query.empty()) {
	enquire->set_query(query);
	// We could use the value of topdoc as first parameter, but we
	// need to know the first few items on the mset to fake a
	// relevance set for topterms.
	//
	// If min_hits isn't set, check at least one extra result so we
	// know if we've reached the end of the matches or not - then we
	// can avoid offering a "next" button which leads to an empty page.
	mset = enquire->get_mset(0, topdoc + hits_per_page,
				 topdoc + max(hits_per_page + 1, min_hits),
				 &rset, mdecider);
    }
    if (usec != -1) {
#ifdef HAVE_GETTIMEOFDAY
	if (gettimeofday(&tv, 0) == 0) {
	    sec = tv.tv_sec - sec;
	    usec = tv.tv_usec - usec;
	    if (usec < 0) {
		--sec;
		usec += 1000000;
	    }
	} else {
	    usec = -1;
	}
#elif defined(FTIME_RETURNS_VOID)
	ftime(&tp);
	sec = tp.time - sec;
	usec = tp.millitm * 1000 - usec;
	if (usec < 0) {
	    --sec;
	    usec += 1000000;
	}
#elif defined(HAVE_FTIME)
	if (ftime(&tp) == 0) {
	    sec = tp.time - sec;
	    usec = tp.millitm * 1000 - usec;
	    if (usec < 0) {
		--sec;
		usec += 1000000;
	    }
	} else {
	    usec = -1;
	}
#else
	usec = time(NULL);
	if (usec != -1) {
	    sec = sec - usec;
	    usec = 0;
	}
#endif
    }
}

static string
percent_encode(const string &str)
{
    string res;
    const char *p = str.c_str();
    while (true) {
	unsigned char ch = *p++;
	if (ch == 0) return res;
	if (ch <= 32 || ch >= 127 || strchr("#%&+,/:;<=>?@[\\]^_{|}", ch)) {
	    char buf[4];
	    my_snprintf(buf, sizeof(buf), "%%%02x", ch);
	    res.append(buf, 3);
	} else {
	    res += ch;
	}
    }
}

static string
html_escape(const string &str)
{
    string res;
    string::size_type p = 0;
    while (p < str.size()) {
	char ch = str[p++];
	switch (ch) {
	    case '<':
	        res += "&lt;";
	        continue;
	    case '>':
	        res += "&gt;";
	        continue;
	    case '&':
	        res += "&amp;";
	        continue;
	    case '"':
	        res += "&quot;";
	        continue;
	    default:
	        res += ch;
	}
    }
    return res;
}

static string
html_strip(const string &str)
{
    string res;
    string::size_type p = 0;
    bool skip = false;
    while (p < str.size()) {
	char ch = str[p++];
	switch (ch) {
	    case '<':
	        skip = true;
	        continue;
	    case '>':
	        skip = false;
	        continue;
	    default:
	        if (! skip) res += ch;
	}
    }
    return res;
}

// FIXME split list into hash or map and use that rather than linear lookup?
static int word_in_list(const string& word, const string& list)
{
    string::size_type split = 0, split2;
    int count = 0;
    while ((split2 = list.find('\t', split)) != string::npos) {
	if (word.size() == split2 - split) {
	    if (memcmp(word.data(), list.data() + split, word.size()) == 0)
		return count;
	}
	split = split2 + 1;
	++count;
    }
    if (word.size() == list.size() - split) {
	if (memcmp(word.data(), list.data() + split, word.size()) == 0)
	    return count;
    }
    return -1;
}

// Not a character in an identifier
inline static bool
p_notid(unsigned int c)
{
    return !isalnum(static_cast<unsigned char>(c)) && c != '_';
}

// Not a character in an HTML tag name
inline static bool
p_nottag(unsigned int c)
{
    return !isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-';
}

inline static bool
p_plusminus(unsigned int c)
{
    return c == '+' || c == '-';
}

// FIXME: shares algorithm with indextext.cc!
static string
html_highlight(const string &s, const string &list,
	       const string &bra, const string &ket)
{
    if (!stemmer) {
	stemmer = new Xapian::Stem(option["stemmer"]);
    }

    string res;

    Utf8Iterator j(s);
    const Utf8Iterator s_end;
    while (true) {
	Utf8Iterator first = j;
	while (first != s_end && !is_wordchar(*first)) ++first;
	if (first == s_end) break;
	Utf8Iterator term_end;
	string term;
	string word;
	const char *l = j.raw();
	if (*first < 128 && isupper(*first)) {
	    j = first;
	    Xapian::Unicode::append_utf8(term, *j);
	    while (++j != s_end && *j == '.' && ++j != s_end && *j < 128 && isupper(*j)) {
		Xapian::Unicode::append_utf8(term, *j);
	    }
	    if (term.length() < 2 || (j != s_end && is_wordchar(*j))) {
		term = "";
	    }
	    term_end = j;
	}
	if (term.empty()) {
	    j = first;
	    while (is_wordchar(*j)) {
		Xapian::Unicode::append_utf8(term, *j);
		++j;
		if (j == s_end) break;
		if (*j == '&' || *j == '\'') {
		    Utf8Iterator next = j;
		    ++next;
		    if (next == s_end || !is_wordchar(*next)) break;
		    term += *j;
		    j = next;
		}
	    }
	    term_end = j;
	    if (j != s_end && (*j == '+' || *j == '-' || *j == '#')) {
		string::size_type len = term.length();
		if (*j == '#') {
		    term += '#';
		    do { ++j; } while (j != s_end && *j == '#');
		} else {
		    while (j != s_end && (*j == '+' || *j == '-')) {
			Xapian::Unicode::append_utf8(term, *j);
			++j;
		    }
		}
		if (term.size() - len > 3 || (j != s_end && is_wordchar(*j))) {
		    term.resize(len);
		} else {
		    term_end = j;
		}
	    }
	}
	j = term_end;
	unsigned char first_char = term[0];
	term = Xapian::Unicode::tolower(term);
	int match = -1;
	if (first_char < 128 && isupper(first_char)) {
	    match = word_in_list('R' + term, list);
	}
	if (match == -1)
	    match = word_in_list((*stemmer)(term), list);
	if (match >= 0) {
	    res += html_escape(string(l, first.raw() - l));
	    if (!bra.empty()) {
		res += bra;
	    } else {
		static const char * colours[] = {
		    "ffff66", "66ffff", "ff66ff", "6666ff", "ff6666",
		    "66ff66", "ffaa33", "33ffaa", "aa33ff", "33aaff"
		};
		res += "<b style=\"color:black;background-color:#";
		res += colours[match % (sizeof(colours) / sizeof(colours[0]))];
		res += "\">";
	    }
	    word = string(first.raw(), j.raw() - first.raw());
	    res += html_escape(word);
	    if (!bra.empty()) {
		res += ket;
	    } else {
		res += "</b>";
	    }
	} else {
	    res += html_escape(string(l, j.raw() - l));
	}
    }
    if (j != s_end) res += html_escape(string(j.raw(), j.left()));
    return res;
}

#if 0
static void
print_query_string(const char *after)
{
    if (after && strncmp(after, "&B=", 3) == 0) {
	char prefix = after[3];
	string::size_type start = 0, amp = 0;
	while (true) {
	    amp = url_query_string.find('&', amp);
	    if (amp == string::npos) {
		cout << url_query_string.substr(start);
		return;
	    }
	    amp++;
	    while (url_query_string[amp] == 'B' &&
		   url_query_string[amp + 1] == '=' &&
		   url_query_string[amp + 2] == prefix) {
		cout << url_query_string.substr(start, amp - start - 1);
		start = url_query_string.find('&', amp + 3);
		if (start == string::npos) return;
		amp = start + 1;
	    }
	}
    }
    cout << url_query_string;
}
#endif

static map<string, string> field;
static Xapian::docid q0;
static Xapian::doccount hit_no;
static int percent;
static Xapian::weight weight;
static Xapian::doccount collapsed;

static string print_caption(const string &fmt, const vector<string> &param);

enum tagval {
CMD_,
CMD_add,
CMD_allterms,
CMD_and,
CMD_cgi,
CMD_cgilist,
CMD_collapsed,
CMD_date,
CMD_dbname,
CMD_dbsize,
CMD_def,
CMD_defaultop,
CMD_div,
CMD_eq,
CMD_env,
CMD_error,
CMD_field,
CMD_filesize,
CMD_filters,
CMD_filterterms,
CMD_find,
CMD_fmt,
CMD_freq,
CMD_ge,
CMD_gt,
CMD_highlight,
CMD_hit,
CMD_hitlist,
CMD_hitsperpage,
CMD_hostname,
CMD_html,
CMD_htmlstrip,
CMD_httpheader,
CMD_id,
CMD_if,
CMD_include,
CMD_last,
CMD_lastpage,
CMD_le,
CMD_length,
CMD_list,
CMD_log,
CMD_lookup,
CMD_lt,
CMD_map,
CMD_max,
CMD_min,
CMD_mod,
CMD_msize,
CMD_msizeexact,
CMD_mul,
CMD_ne,
CMD_nice,
CMD_not,
CMD_now,
CMD_opt,
CMD_or,
CMD_pack,
CMD_percentage,
CMD_prettyterm,
CMD_query,
CMD_querydescription,
CMD_queryterms,
CMD_range,
CMD_record,
CMD_relevant,
CMD_relevants,
CMD_score,
CMD_set,
CMD_setmap,
CMD_setrelevant,
CMD_slice,
CMD_split,
CMD_stoplist,
CMD_sub,
CMD_substr,
CMD_terms,
CMD_thispage,
CMD_time,
CMD_topdoc,
CMD_topterms,
#ifdef HAVE_PCRE
CMD_transform,
#endif
CMD_uniq,
CMD_unpack,
CMD_unstem,
CMD_url,
CMD_value,
CMD_version,
CMD_weight,
CMD_MACRO // special tag for macro evaluation
};

struct func_attrib {
    int tag;
    int minargs, maxargs, evalargs;
    char ensure;
};

#define STRINGIZE(N) _STRINGIZE(N)
#define _STRINGIZE(N) #N

#define T(F,A,B,C,D) {STRINGIZE(F),{CMD_##F,A,B,C,D}}
struct func_desc {
    const char *name;
    struct func_attrib a;
};

#define N -1
#define M 'M'
#define Q 'Q'
// NB when adding a new command which ensures M or Q, update the list in
// docs/omegascript.txt
static struct func_desc func_tab[] = {
//name minargs maxargs evalargs ensure
{"",{CMD_,	   N, N, 0, 0}},// commented out code
T(add,		   0, N, N, 0), // add a list of numbers
T(allterms,	   0, 1, N, 0), // list of all terms matching document
T(and,		   1, N, 0, 0), // logical shortcutting and of a list of values
T(cgi,		   1, 1, N, 0), // return cgi parameter value
T(cgilist,	   1, 1, N, 0), // return list of values for cgi parameter
T(collapsed,	   0, 0, N, 0), // return number of hits collapsed into this
T(date,		   1, 2, N, 0), // convert time_t to strftime format
				// (default: YYYY-MM-DD)
T(dbname,	   0, 0, N, 0), // database name
T(dbsize,	   0, 0, N, 0), // database size (# of documents)
T(def,		   2, 2, 1, 0), // define a macro
T(defaultop,	   0, 0, N, 0), // default operator: "and" or "or"
T(div,		   2, 2, N, 0), // integer divide
T(env,		   1, 1, N, 0), // environment variable
T(error,	   0, 0, N, 0), // error message
T(eq,		   2, 2, N, 0), // test equality
T(field,	   1, 1, N, 0), // lookup field in record
T(filesize,	   1, 1, N, 0), // pretty printed filesize
T(filters,	   0, 0, N, 0), // serialisation of current filters
T(filterterms,	   1, 1, N, 0), // list of terms with a given prefix
T(find,		   2, 2, N, 0), // find entry in list
T(fmt,		   0, 0, N, 0), // name of current format
T(freq,		   1, 1, N, 0), // frequency of a term
T(ge,		   2, 2, N, 0), // test >=
T(gt,		   2, 2, N, 0), // test >
T(highlight,	   2, 4, N, 0), // html escape and highlight words from list
T(hit,		   0, 0, N, 0), // hit number of current mset entry (starting
				// from 0
T(hitlist,	   1, 1, 0, M), // display hitlist using format in argument
T(hitsperpage,	   0, 0, N, 0), // hits per page
T(hostname,	   1, 1, N, 0), // extract hostname from URL
T(html,		   1, 1, N, 0), // html escape string (<>&")
T(htmlstrip,	   1, 1, N, 0), // html strip tags string (s/<[^>]*>?//g)
T(httpheader,      2, 2, N, 0), // arbitrary HTTP header
T(id,		   0, 0, N, 0), // docid of current doc
T(if,		   2, 3, 1, 0), // conditional
T(include,	   1, 1, 1, 0), // include another file
T(last,		   0, 0, N, M), // m-set number of last hit on page
T(lastpage,	   0, 0, N, M), // number of last hit page
T(le,		   2, 2, N, 0), // test <=
T(length,	   1, 1, N, 0), // length of list
T(list,		   2, 5, N, 0), // pretty print list
T(log,		   1, 2, 1, 0), // create a log entry
T(lookup,	   2, 2, N, 0), // lookup in named cdb file
T(lt,		   2, 2, N, 0), // test <
T(map,		   1, 2, 1, 0), // map a list into another list
T(max,		   1, N, N, 0), // maximum of a list of values
T(min,		   1, N, N, 0), // minimum of a list of values
T(mod,		   2, 2, N, 0), // integer modulus
T(msize,	   0, 0, N, M), // number of matches
T(msizeexact,	   0, 0, N, M), // is $msize exact?
T(mul,		   2, N, N, 0), // multiply a list of numbers
T(ne,		   2, 2, N, 0), // test not equal
T(nice,		   1, 1, N, 0), // pretty print integer (with thousands sep)
T(not,		   1, 1, N, 0), // logical not
T(now,		   0, 0, N, 0), // current date/time as a time_t
T(opt,		   1, 2, N, 0), // lookup an option value
T(or,		   1, N, 0, 0), // logical shortcutting or of a list of values
T(pack,		   1, 1, N, 0), // convert a number to a 4 byte big endian binary string
T(percentage,	   0, 0, N, 0), // percentage score of current hit
T(prettyterm,	   1, 1, N, Q), // pretty print term name
T(query,	   0, 0, N, Q), // query
T(querydescription,0, 0, N, Q), // query.get_description()
T(queryterms,	   0, 0, N, Q), // list of query terms
T(range,	   2, 2, N, 0), // return list of values between start and end
T(record,	   0, 1, N, 0), // record contents of document
T(relevant,	   0, 1, N, Q), // is document relevant?
T(relevants,	   0, 0, N, Q), // return list of relevant documents
T(score,	   0, 0, N, 0), // score (0-10) of current hit
T(set,		   2, 2, N, 0), // set option value
T(setmap,	   1, N, N, 0), // set map of option values
T(setrelevant,     0, 1, N, Q), // set rset
T(slice,	   2, 2, N, 0), // slice a list using a second list
T(split,	   1, 2, N, 0), // split a string to give a list
T(stoplist,	   0, 0, N, Q), // return list of stopped terms
T(sub,		   2, 2, N, 0), // subtract
T(substr,	   2, 3, N, 0), // substring
T(terms,	   0, 0, N, M), // list of matching terms
T(thispage,	   0, 0, N, M), // page number of current page
T(time,		   0, 0, N, M), // how long the match took (in seconds)
T(topdoc,	   0, 0, N, M), // first document on current page of hit list
				// (counting from 0)
T(topterms,	   0, 1, N, M), // list of up to N top relevance feedback terms
				// (default 16)
#ifdef HAVE_PCRE
T(transform,	   3, 3, N, 0), // transform with a regexp
#endif
T(uniq,		   1, 1, N, 0), // removed duplicates from a sorted list
T(unpack,	   1, 1, N, 0), // convert 4 byte big endian binary string to a number
T(unstem,	   1, 1, N, Q), // return list of probabilistic terms from
				// the query which stemmed to this term
T(url,		   1, 1, N, 0), // url encode argument
T(value,	   1, 2, N, 0), // return document value
T(version,	   0, 0, N, 0), // omega version string
T(weight,	   0, 0, N, 0), // weight of the current hit
{ NULL,{0,	   0, 0, 0, 0}}
};

#undef T // Leaving T defined screws up Sun's C++ compiler!

static vector<string> macros;

static string
eval(const string &fmt, const vector<string> &param)
{
    static map<string, const struct func_attrib *> func_map;
    if (func_map.empty()) {
	struct func_desc *p;
	for (p = func_tab; p->name != NULL; p++) {
	    func_map[string(p->name)] = &(p->a);
	}
    }
    string res;
    string::size_type p = 0, q;
    while ((q = fmt.find('$', p)) != string::npos) try {
	res += fmt.substr(p, q - p);
	string::size_type code_start = q; // note down for error reporting
	q++;
	if (q >= fmt.size()) break;
	unsigned char ch = fmt[q];
	switch (ch) {
	    // Magic sequences:
	    // `$$' -> `$', `$(' -> `{', `$)' -> `}', `$.' -> `,'
	    case '$':
		res += '$';
		p = q + 1;
		continue;
	    case '(':
		res += '{';
		p = q + 1;
		continue;
	    case ')':
		res += '}';
		p = q + 1;
		continue;
	    case '.':
		res += ',';
		p = q + 1;
		continue;
	    case '_':
		ch = '0';
		// FALL THRU
	    case '1': case '2': case '3': case '4': case '5':
	    case '6': case '7': case '8': case '9':
		ch -= '0';
		if (ch < param.size()) res += param[ch];
		p = q + 1;
		continue;
	    case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
	    case 'g': case 'h': case 'i': case 'j': case 'k': case 'l':
	    case 'm': case 'n': case 'o': case 'p': case 'q': case 'r':
	    case 's': case 't': case 'u': case 'v': case 'w': case 'x':
	    case 'y': case 'z':
	    case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
	    case 'G': case 'H': case 'I': case 'J': case 'K': case 'L':
	    case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R':
	    case 'S': case 'T': case 'U': case 'V': case 'W': case 'X':
	    case 'Y': case 'Z':
	    case '{':
		break;
	    default:
		string msg = "Unknown $ code in: $" + fmt.substr(q);
		throw msg;
	}
	p = find_if(fmt.begin() + q, fmt.end(), p_notid) - fmt.begin();
	string var = fmt.substr(q, p - q);
	map<string, const struct func_attrib *>::const_iterator func;
	func = func_map.find(var);
	if (func == func_map.end()) {
	    throw "Unknown function `" + var + "'";
	}
	vector<string> args;
	if (fmt[p] == '{') {
	    q = p + 1;
	    int nest = 1;
	    while (true) {
		p = fmt.find_first_of(",{}", p + 1);
		if (p == string::npos)
		    throw "missing } in " + fmt.substr(code_start);
		if (fmt[p] == '{') {
		    ++nest;
		} else {
		    if (nest == 1) {
			// should we split the args
			if (func->second->minargs != N) {
			    args.push_back(fmt.substr(q, p - q));
			    q = p + 1;
			}
		    }
		    if (fmt[p] == '}' && --nest == 0) break;
		}
	    }
	    if (func->second->minargs == N)
		args.push_back(fmt.substr(q, p - q));
	    p++;
	}

	if (func->second->minargs != N) {
	    if ((int)args.size() < func->second->minargs)
		throw "too few arguments to $" + var;
	    if (func->second->maxargs != N &&
		(int)args.size() > func->second->maxargs)
		throw "too many arguments to $" + var;

	    vector<string>::size_type n;
	    if (func->second->evalargs != N)
		n = func->second->evalargs;
	    else
		n = args.size();

	    for (vector<string>::size_type j = 0; j < n; j++)
		args[j] = eval(args[j], param);
	}
	if (func->second->ensure == 'Q' || func->second->ensure == 'M')
	    ensure_query_parsed();
	if (func->second->ensure == 'M') ensure_match();
	string value;
	switch (func->second->tag) {
	    case CMD_:
	        break;
	    case CMD_add: {
		int total = 0;
		vector<string>::const_iterator i;
		for (i = args.begin(); i != args.end(); i++)
		    total += string_to_int(*i);
		value = int_to_string(total);
		break;
	    }
	    case CMD_allterms: {
		// list of all terms indexing document
		int id = q0;
		if (!args.empty()) id = string_to_int(args[0]);
		Xapian::TermIterator term = db.termlist_begin(id);
		for ( ; term != db.termlist_end(id); term++)
		    value = value + *term + '\t';

		if (!value.empty()) value.erase(value.size() - 1);
		break;
	    }
	    case CMD_and: {
		value = "true";
		for (vector<string>::const_iterator i = args.begin();
		     i != args.end(); i++) {
		    if (eval(*i, param).empty()) {
			value = "";
			break;
		    }
	        }
		break;
	    }
	    case CMD_cgi: {
		MCI i = cgi_params.find(args[0]);
		if (i != cgi_params.end()) value = i->second;
		break;
	    }
	    case CMD_cgilist: {
		pair<MCI, MCI> g;
		g = cgi_params.equal_range(args[0]);
		for (MCI i = g.first; i != g.second; i++)
		    value = value + i->second + '\t';
		if (!value.empty()) value.erase(value.size() - 1);
		break;
	    }
	    case CMD_collapsed: {
		value = int_to_string(collapsed);
		break;
	    }
	    case CMD_date:
		value = args[0];
		if (!value.empty()) {
		    char buf[64] = "";
		    time_t date = string_to_int(value);
		    if (date != (time_t)-1) {
			struct tm *then;
			then = gmtime(&date);
			string date_fmt = "%Y-%m-%d";
			if (args.size() > 1) date_fmt = eval(args[1], param);
			strftime(buf, sizeof buf, date_fmt.c_str(), then);
		    }
		    value = buf;
		}
		break;
	    case CMD_dbname:
		value = dbname;
		break;
	    case CMD_dbsize: {
		static Xapian::doccount dbsize;
		if (!dbsize) dbsize = db.get_doccount();
		value = int_to_string(dbsize);
		break;
	    }
	    case CMD_def: {
		func_attrib *fa = new func_attrib;
		fa->tag = CMD_MACRO + macros.size();
		fa->minargs = 0;
		fa->maxargs = 9;
		fa->evalargs = N; // FIXME: or 0?
		fa->ensure = 0;

		macros.push_back(args[1]);
		func_map[args[0]] = fa;
		break;
	    }
	    case CMD_defaultop:
		if (default_op == Xapian::Query::OP_AND) {
		    value = "and";
		} else {
		    value = "or";
		}
		break;
	    case CMD_div: {
		int denom = string_to_int(args[1]);
		if (denom == 0) {
		    value = "divide by 0";
		} else {
		    value = int_to_string(string_to_int(args[0]) /
					  string_to_int(args[1]));
		}
		break;
	    }
	    case CMD_eq:
		if (args[0] == args[1]) value = "true";
		break;
	    case CMD_env: {
		char *env = getenv(args[0].c_str());
		if (env != NULL) value = env;
		break;
	    }
	    case CMD_error:
		if (error_msg.empty() && enquire == NULL && !dbname.empty()) {
		    error_msg = "Database `" + dbname + "' couldn't be opened";
		}
		value = error_msg;
		break;
	    case CMD_field:
		value = field[args[0]];
		break;
	    case CMD_filesize: {
		// FIXME: rounding?  i18n?
		int size = string_to_int(args[0]);
		int intpart = size;
		int fraction = -1;
		const char * format = 0;
		if (size < 0) {
		    // Negative size -> empty result.
		} else if (size == 1) {
		    format = "%d byte";
		} else if (size < 1024) {
		    format = "%d bytes";
		} else if (size < 1024*1024) {
		    intpart = size / 1024;
		    fraction = size % 1024;
		    format = "%d.%cK";
		} else if (size < 1024*1024*1024) {
		    intpart = size / (1024 * 1024);
		    fraction = size % (1024 * 1024);
		    format = "%d.%cM";
		} else {
		    intpart = size / (1024 * 1024 * 1024);
		    fraction = size % (1024 * 1024 * 1024);
		    format = "%d.%cG";
		}
		if (format) {
		    char buf[200];
		    int len;
		    if (fraction == -1) {
			len = my_snprintf(buf, sizeof(buf), format, intpart);
		    } else {
			fraction = (fraction * 10 / 1024) + '0';
			len = my_snprintf(buf, sizeof(buf), format, intpart, fraction);
		    }
		    if (len < 0 || (unsigned)len > sizeof(buf)) len = sizeof(buf);
		    value.assign(buf, len);
		}
		break;
	    }
	    case CMD_filters:
		value = filters;
		break;
	    case CMD_filterterms: {
		Xapian::TermIterator term = db.allterms_begin();
		term.skip_to(args[0]);
		while (term != db.allterms_end()) {
		    string t = *term;
		    if (t.substr(0, args[0].size()) != args[0]) break;
		    value = value + t + '\t';
		    ++term;
		}

		if (!value.empty()) value.erase(value.size() - 1);
		break;
	    }
	    case CMD_find: {
		string l = args[0], s = args[1];
		string::size_type i = 0, j = 0;
		size_t count = 0;
		while (j != l.size()) {
		    j = l.find('\t', i);
		    if (j == string::npos) j = l.size();
		    if (j - i == s.length()) {
			if (memcmp(s.data(), l.data() + i, j - i) == 0) {
			    value = int_to_string(count);
			    break;
			}
		    }
		    ++count;
		    i = j + 1;
		}
		break;
	    }
	    case CMD_fmt:
		value = fmtname;
		break;
	    case CMD_freq:
		try {
		    value = int_to_string(mset.get_termfreq(args[0]));
		} catch (...) {
		    value = int_to_string(db.get_termfreq(args[0]));
		}
		break;
            case CMD_ge:
		if (string_to_int(args[0]) >= string_to_int(args[1]))
		    value = "true";
		break;
            case CMD_gt:
		if (string_to_int(args[0]) > string_to_int(args[1]))
		    value = "true";
		break;
	    case CMD_highlight: {
		string bra, ket;
		if (args.size() > 2) {
		    bra = args[2];
		    if (args.size() > 3) {
			ket = args[3];
		    } else {
			string::const_iterator i;
			i = find_if(bra.begin() + 2, bra.end(), p_nottag);
			ket = "</";
			ket += bra.substr(1, i - bra.begin() - 1);
			ket += '>';
		    }
		}

		value = html_highlight(args[0], args[1], bra, ket);
		break;
	    }
	    case CMD_hit:
		// 0-based mset index
		value = int_to_string(hit_no);
		break;
	    case CMD_hitlist:
#if 0
		const char *q;
		int ch;

		url_query_string = "?DB=";
		url_query_string += dbname;
		url_query_string += "&P=";
		q = query_string.c_str();
		while ((ch = *q++) != '\0') {
		    switch (ch) {
		     case '+':
			url_query_string += "%2b";
			break;
		     case '"':
			url_query_string += "%22";
			break;
		     case ' ':
			ch = '+';
			/* fall through */
		     default:
			url_query_string += ch;
		    }
		}
	        // add any boolean terms
		for (FMCI i = filter_map.begin(); i != filter_map.end(); i++) {
		    url_query_string += "&B=";
		    url_query_string += i->second;
		}
#endif
		for (hit_no = topdoc; hit_no < last; hit_no++)
		    value += print_caption(args[0], param);
		hit_no = 0;
		break;
	    case CMD_hitsperpage:
		value = int_to_string(hits_per_page);
		break;
	    case CMD_hostname: {
	        value = args[0];
		// remove URL scheme and/or path
		string::size_type i = value.find("://");
		if (i == string::npos) i = 0; else i += 3;
		value = value.substr(i, value.find('/', i) - i);
		// remove user@ or user:password@
		i = value.find('@');
		if (i != string::npos) value = value.substr(i + 1);
		// remove :port
		i = value.find(':');
		if (i != string::npos) value = value.substr(0, i);
		break;
	    }
	    case CMD_html:
	        value = html_escape(args[0]);
		break;
	    case CMD_htmlstrip:
	        value = html_strip(args[0]);
		break;
	    case CMD_httpheader:
		cout << args[0] << ": " << args[1] << endl;
		if (!set_content_type && args[0].length() == 12 &&
			strcasecmp(args[0].c_str(), "Content-Type") == 0) {
		    set_content_type = true;
		}
	        break;
	    case CMD_id:
		// document id
		value = int_to_string(q0);
		break;
	    case CMD_if:
		if (!args[0].empty())
		    value = eval(args[1], param);
		else if (args.size() > 2)
		    value = eval(args[2], param);
		break;
	    case CMD_include:
	        value = eval_file(args[0]);
	        break;
	    case CMD_last:
		value = int_to_string(last);
		break;
	    case CMD_lastpage: {
		int l = mset.get_matches_estimated();
		if (l > 0) l = (l - 1) / hits_per_page + 1;
		value = int_to_string(l);
		break;
	    }
            case CMD_le:
		if (string_to_int(args[0]) <= string_to_int(args[1]))
		    value = "true";
		break;
            case CMD_length:
		if (args[0].empty()) {
		    value = "0";
		} else {
		    size_t length = count(args[0].begin(), args[0].end(), '\t');
		    value = int_to_string(length + 1);
		}
		break;
	    case CMD_list: {
		if (!args[0].empty()) {
		    string pre, inter, interlast, post;
		    switch (args.size()) {
		     case 2:
			inter = interlast = args[1];
			break;
		     case 3:
			inter = args[1];
			interlast = args[2];
			break;
		     case 4:
			pre = args[1];
			inter = interlast = args[2];
			post = args[3];
			break;
		     case 5:
			pre = args[1];
			inter = args[2];
			interlast = args[3];
			post = args[4];
			break;
		    }
		    value += pre;
		    string list = args[0];
		    string::size_type split = 0, split2;
		    while ((split2 = list.find('\t', split)) != string::npos) {
			if (split) value += inter;
			value += list.substr(split, split2 - split);
			split = split2 + 1;
		    }
		    if (split) value += interlast;
		    value += list.substr(split);
		    value += post;
		}
		break;
	    }
	    case CMD_log: {
		if (!vet_filename(args[0])) break;
		string logfile = log_dir + args[0];
	        int fd = open(logfile.c_str(), O_CREAT|O_APPEND|O_WRONLY, 0644);
		if (fd == -1) break;
		vector<string> noargs;
		noargs.resize(1);
		string line;
		if (args.size() > 1) {
		    line = args[1];
		} else {
		    line = DEFAULT_LOG_ENTRY;
		}
		line = eval(line, noargs);
		line += '\n';
		write(fd, line.data(), line.length());
		close(fd);
		break;
	    }
	    case CMD_lookup: {
		if (!vet_filename(args[0])) break;
		string cdbfile = cdb_dir + args[0];
	        int fd = open(cdbfile.c_str(), O_RDONLY);
		if (fd == -1) break;

		struct cdb cdb;
		cdb_init(&cdb, fd);

		if (cdb_find(&cdb, args[1].data(), args[1].length()) > 0) {
		    size_t datalen = cdb_datalen(&cdb);
		    const void *dat = cdb_get(&cdb, datalen, cdb_datapos(&cdb));
		    if (q) {
			value = string(static_cast<const char *>(dat), datalen);
		    }
		}

		cdb_free(&cdb);
		close(fd); // FIXME: cache fds?
		break;
	    }
            case CMD_lt:
		if (string_to_int(args[0]) < string_to_int(args[1]))
		    value = "true";
		break;
	    case CMD_map:
		if (!args[0].empty()) {
		    string l = args[0], pat = args[1];
		    vector<string> new_args(param);
		    string::size_type i = 0, j;
		    while (true) {
			j = l.find('\t', i);
			new_args[0] = l.substr(i, j - i);
			value += eval(pat, new_args);
			if (j == string::npos) break;
			value += '\t';
			i = j + 1;
		    }
		}
	        break;
	    case CMD_max: {
		vector<string>::const_iterator i = args.begin();
		int val = string_to_int(*i++);
		for (; i != args.end(); i++) {
		    int x = string_to_int(*i);
		    if (x > val) val = x;
	        }
		value = int_to_string(val);
		break;
	    }
	    case CMD_min: {
		vector<string>::const_iterator i = args.begin();
		int val = string_to_int(*i++);
		for (; i != args.end(); i++) {
		    int x = string_to_int(*i);
		    if (x < val) val = x;
	        }
		value = int_to_string(val);
		break;
	    }
	    case CMD_msize:
		// number of matches
		value = int_to_string(mset.get_matches_estimated());
		break;
	    case CMD_msizeexact:
		// is msize exact?
		if (mset.get_matches_lower_bound()
		    == mset.get_matches_upper_bound())
		    value = "true";
		break;
	    case CMD_mod: {
		int denom = string_to_int(args[1]);
		if (denom == 0) {
		    value = "divide by 0";
		} else {
		    value = int_to_string(string_to_int(args[0]) %
					  string_to_int(args[1]));
		}
		break;
	    }
	    case CMD_mul: {
		vector<string>::const_iterator i = args.begin();
		int total = string_to_int(*i++);
		while (i != args.end())
		    total *= string_to_int(*i++);
		value = int_to_string(total);
		break;
	    }
            case CMD_ne:
		if (args[0] != args[1]) value = "true";
		break;
	    case CMD_nice: {
		string::const_iterator i = args[0].begin();
		int len = args[0].length();
		while (len) {
		    value += *i++;
		    if (--len && len % 3 == 0) value += option["thousand"];
		}
		break;
	    }
	    case CMD_not:
		if (args[0].empty()) value = "true";
		break;
	    case CMD_now: {
		char buf[64];
		my_snprintf(buf, sizeof(buf), "%lu", (unsigned long)time(NULL));
		// MSVC's snprintf omits the zero byte if the string if
		// sizeof(buf) long.
		buf[sizeof(buf) - 1] = '\0';
		value = buf;
		break;
	    }
	    case CMD_opt:
		if (args.size() == 2) {
		    value = option[args[0] + "," + args[1]];
		} else {
		    value = option[args[0]];
		}
		break;
	    case CMD_or: {
		for (vector<string>::const_iterator i = args.begin();
		     i != args.end(); i++) {
		    value = eval(*i, param);
		    if (!value.empty()) break;
	        }
		break;
	    }
	    case CMD_pack:
		value = int_to_binary_string(string_to_int(args[0]));
		break;
	    case CMD_percentage:
		// percentage score
		value = int_to_string(percent);
		break;
	    case CMD_prettyterm:
		value = pretty_term(args[0]);
		break;
	    case CMD_query:
		value = query_string;
		break;
	    case CMD_querydescription:
		value = query.get_description();
		break;
	    case CMD_queryterms:
		value = queryterms;
		break;
	    case CMD_range: {
		int start = string_to_int(args[0]);
		int end = string_to_int(args[1]);
	        while (start <= end) {
		    value = value + int_to_string(start);
		    if (start < end) value += '\t';
		    start++;
		}
		break;
	    }
	    case CMD_record: {
		int id = q0;
		if (!args.empty()) id = string_to_int(args[0]);
		value = db.get_document(id).get_data();
		break;
	    }
	    case CMD_relevant: {
		// document id if relevant; empty otherwise
		int id = q0;
		if (!args.empty()) id = string_to_int(args[0]);
		map<Xapian::docid, bool>::iterator i = ticked.find(id);
		if (i != ticked.end()) {
		    i->second = false; // icky side-effect
		    value = int_to_string(id);
		}
		break;
	    }
	    case CMD_relevants:	{
		for (map <Xapian::docid, bool>::const_iterator i = ticked.begin();
		     i != ticked.end(); i++) {
		    if (i->second) value += int_to_string(i->first) + '\t';
		}
		if (!value.empty()) value.erase(value.size() - 1);
		break;
	    }
	    case CMD_score:
	        // Score (0 to 10)
		value = int_to_string(percent / 10);
		break;
	    case CMD_set:
		option[args[0]] = args[1];
		break;
	    case CMD_setmap: {
		string base = args[0] + ',';
		if (args.size() % 2 != 1)
		    throw string("$setmap requires an odd number of arguments");
		for (unsigned int i = 1; i + 1 < args.size(); i += 2) {
		    option[base + args[i]] = args[i + 1];
		}
		break;
	    }
	    case CMD_setrelevant: {
		string::size_type i = 0, j;
		while (true) {
		    j = args[0].find_first_not_of("0123456789", i);
		    Xapian::docid id = atoi(args[0].substr(i, j - i).c_str());
		    if (id) {
			rset.add_document(id);
			ticked[id] = true;
		    }
		    if (j == string::npos) break;
		    i = j + 1;
		}
		break;
	    }
	    case CMD_slice: {
		string list = args[0], pos = args[1];
		vector<string> items;
		string::size_type i = 0, j;
		while (true) {
		    j = list.find('\t', i);
		    items.push_back(list.substr(i, j - i));
		    if (j == string::npos) break;
		    i = j + 1;
		}
		i = 0;
		bool have_added = false;
		while (true) {
		    j = pos.find('\t', i);
		    int item = string_to_int(pos.substr(i, j - i));
		    if (item >= 0 && size_t(item) < items.size()) {
			if (have_added) value += '\t';
			value += items[item];
			have_added = true;
		    }
		    if (j == string::npos) break;
		    i = j + 1;
		}
	        break;
	    }
	    case CMD_split: {
		string split;
		if (args.size() == 1) {
		    split = " ";
		    value = args[0];
		} else {
		    split = args[0];
		    value = args[1];
		}
		string::size_type i = 0;
		while (true) {
		    if (split.empty()) {
			++i;
			if (i >= value.size()) break;
		    } else {
			i = value.find(split, i);
			if (i == string::npos) break;
		    }
		    value.replace(i, split.size(), 1, '\t');
		    ++i;
		}
	        break;
	    }
	    case CMD_stoplist: {
		Xapian::TermIterator i = qp.stoplist_begin();
		Xapian::TermIterator end = qp.stoplist_end();
		while (i != end) {
		    if (!value.empty()) value += '\t';
		    value += *i;
		    ++i;
		}
		break;
	    }
	    case CMD_sub:
		value = int_to_string(string_to_int(args[0]) -
				      string_to_int(args[1]));
		break;
	    case CMD_substr: {
		int start = string_to_int(args[1]);
		if (start < 0) {
		    if (static_cast<size_t>(-start) >= args[0].size()) {
			start = 0;
		    } else {
			start = static_cast<int>(args[0].size()) + start;
		    }
		} else {
		    if (static_cast<size_t>(start) >= args[0].size()) break;
		}
		size_t len = string::npos;
		if (args.size() > 2) {
		    int int_len = string_to_int(args[2]);
		    if (int_len >= 0) {
			len = size_t(int_len);
		    } else {
			len = args[0].size() - start;
			if (static_cast<size_t>(-int_len) >= len) {
			    len = 0;
			} else {
			    len -= static_cast<size_t>(-int_len);
			}
		    }
		}
		value = args[0].substr(start, len);
		break;
	    }
	    case CMD_terms:
		if (enquire) {
		    // list of matching terms
		    Xapian::TermIterator term = enquire->get_matching_terms_begin(q0);
		    while (term != enquire->get_matching_terms_end(q0)) {
			// check term was in the typed query so we ignore
			// boolean filter terms
			if (termset.find(*term) != termset.end())
			    value = value + *term + '\t';
			++term;
		    }

		    if (!value.empty()) value.erase(value.size() - 1);
		}
		break;
	    case CMD_thispage:
		value = int_to_string(topdoc / hits_per_page + 1);
		break;
	    case CMD_time:
		if (usec != -1) {
		    char buf[64];
		    my_snprintf(buf, sizeof(buf), "%ld.%06ld", sec, usec);
		    // MSVC's snprintf omits the zero byte if the string if
		    // sizeof(buf) long.
		    buf[sizeof(buf) - 1] = '\0';
		    value = buf;
		}
		break;
	    case CMD_topdoc:
		// first document on current page of hit list (counting from 0)
		value = int_to_string(topdoc);
		break;
	    case CMD_topterms:
		if (enquire) {
		    int howmany = 16;
		    if (!args.empty()) howmany = string_to_int(args[0]);
		    if (howmany < 0) howmany = 0;

		    // List of expand terms
		    Xapian::ESet eset;
		    ExpandDeciderOmega decider(db);

		    if (!rset.empty()) {
			eset = enquire->get_eset(howmany * 2, rset, &decider);
		    } else if (mset.size()) {
			// invent an rset
			Xapian::RSet tmp;

			int c = 5;
			// FIXME: what if mset does not start at first match?
			Xapian::MSetIterator m = mset.begin();
			for ( ; m != mset.end(); ++m) {
			    tmp.add_document(*m);
			    if (--c == 0) break;
			}

			eset = enquire->get_eset(howmany * 2, tmp, &decider);
		    }

		    Xapian::ESetIterator i;
		    set<string> seen;
		    {
			if (!stemmer) {
			    stemmer = new Xapian::Stem(option["stemmer"]);
			}
			// Exclude terms "similar" to those already in
			// the query
			set<string>::const_iterator t;
			for (t = termset.begin(); t != termset.end(); ++t) {
			    string term = *t;
			    if (term[0] == 'R') {
				term.erase(0, 1);
				term = (*stemmer)(term);
			    }
			    seen.insert(term);
			}
		    }
		    MyStopper stopper;
		    for (i = eset.begin(); i != eset.end(); ++i) {
			string term = *i;
			if (term[0] == 'R') {
			    term.erase(0, 1);
			    if (stopper(term)) continue;
			    term = (*stemmer)(term);
			}
			if (seen.find(term) != seen.end()) continue;
			seen.insert(term);
			value += *i;
			value += '\t';
			if (--howmany == 0) break;
		    }
		    if (!value.empty()) value.erase(value.size() - 1);
		}
		break;
#ifdef HAVE_PCRE
	    case CMD_transform: {
		pcre *re;
		const char *error;
		int erroffset;
		int offsets[30];
		int matches;
		re = pcre_compile(args[0].c_str(), 0, &error, &erroffset, NULL);
		matches = pcre_exec(re, NULL, args[2].data(), args[2].size(),
				    0, 0, offsets, 30);
		if (matches > 0) {
		    string::const_iterator i;
		    value = args[2].substr(0, offsets[0]);
		    for (i = args[1].begin(); i != args[1].end(); ++i) {
			if (*i != '\\') {
			    value += *i;
			} else {
			    ++i;
			    if (i != args[1].end()) {
				if (*i >= '0' && *i < '0' + matches) {
				    int c = (*i - '0') * 2;
				    value.append(args[2].substr(offsets[c],
						offsets[c + 1] - offsets[c]));
				} else {
				    value += *i;
				}
			    }
			}
		    }
		    value += args[2].substr(offsets[1]);
		} else {
		    value = args[2];
		}
		break;
	    }
#endif
	    case CMD_uniq: {
		const string &list = args[0];
		if (list.empty()) break;
		string::size_type split = 0, split2;
		string prev;
		do {
		    split2 = list.find('\t', split);
		    string item = list.substr(split, split2 - split);
		    if (split == 0) {
			value = item;
		    } else if (item != prev) {
			value += '\t';
			value += item;
		    }
		    prev = item;
		    split = split2 + 1;
		} while (split2 != string::npos);
		break;
	    }
	    case CMD_unpack:
		value = int_to_string(binary_string_to_int(args[0]));
		break;
	    case CMD_unstem: {
		const string &term = args[0];
		Xapian::TermIterator i = qp.unstem_begin(term);
		Xapian::TermIterator end = qp.unstem_end(term);
		while (i != end) {
		    if (!value.empty()) value += '\t';
		    value += *i;
		    ++i;
		}
		break;
	    }
	    case CMD_url:
	        value = percent_encode(args[0]);
		break;
	    case CMD_value: {
		Xapian::docid id = q0;
		Xapian::valueno value_no = string_to_int(args[0]);
		if (args.size() > 1) id = string_to_int(args[1]);
		value = db.get_document(id).get_value(value_no);
		break;
	    }
	    case CMD_version:
		value = "Xapian - "PACKAGE" "VERSION;
		break;
	    case CMD_weight:
		value = double_to_string(weight);
		break;
	    default: {
		args.insert(args.begin(), param[0]);
		int macro_no = func->second->tag - CMD_MACRO;
		assert(macro_no >= 0 && (unsigned int)macro_no < macros.size());
		// throw "Unknown function `" + var + "'";
		value = eval(macros[macro_no], args);
		break;
	    }
	}
        res += value;
    } catch (const Xapian::Error & e) {
	// FIXME: this means we only see the most recent error in $error
	// - is that the best approach?
	error_msg = e.get_msg();
    }

    res += fmt.substr(p);
    return res;
}

static string
eval_file(const string &fmtfile)
{
    string err;
    if (vet_filename(fmtfile)) {
	string file = template_dir + fmtfile;
	string fmt;
	if (load_file(file, fmt)) {
	    vector<string> noargs;
	    noargs.resize(1);
	    return eval(fmt, noargs);
	}
	err = strerror(errno);
    } else {
	err = "name contains `..'";
    }

    // FIXME: report why!
    string msg = string("Couldn't read format template `") + fmtfile + '\'';
    if (!err.empty()) msg += " (" + err + ')';
    throw msg;
}

extern string
pretty_term(const string & term)
{
    if (term.empty()) return term;

    if (term.length() >= 2 && term[0] == 'R') {
	string result = term.substr(1);
	result[0] = toupper(static_cast<unsigned char>(result[0]));
	return result;
    }

    // If there's an unstemmed version in the query, use that.
    // If the multiple forms with the same stem appear, we arbitrarily pick
    // the first.
    Xapian::TermIterator i = qp.unstem_begin(term);
    if (i != qp.unstem_end(term)) return *i;

    // If the term wasn't indexed unstemmed, it's probably a non-term
    // e.g. "litr" - the stem of "litre"
    // FIXME: perhaps ought to check termfreq > some threshold
    if (!db.term_exists('R' + term))
	return '"' + term + '"';

    if (!stemmer) {
	stemmer = new Xapian::Stem(option["stemmer"]);
    }

    // The term is present unstemmed, but if it would stem further it still
    // needs protecting.
    if ((*stemmer)(term) != term)
	return '"' + term + '"';

    return term;
}

static string
print_caption(const string &fmt, const vector<string> &param)
{
    q0 = *(mset[hit_no]);

    weight = mset[hit_no].get_weight();
    percent = mset.convert_to_percent(mset[hit_no]);
    collapsed = mset[hit_no].get_collapse_count();

    Xapian::Document doc = db.get_document(q0);
    string text = doc.get_data();

    // Parse document data.
    field.clear();
    string::size_type i = 0;
    string fieldnames = option["fieldnames"];
    if (!fieldnames.empty()) {
	// Each line is a field, with fieldnames taken from corresponding
	// entries in the tab-separated list specified by $opt{fieldnames}.
	string::size_type n = 0, n2;
	while (true) {
	    n2 = fieldnames.find('\t', n);
	    string::size_type old_i = i;
	    i = text.find('\n', i);
	    field[fieldnames.substr(n, n2 - n)] = text.substr(old_i, i - old_i);
	    if (n2 == string::npos || i == string::npos) break;
	    ++i;
	    n = n2 + 1;
	}
    } else {
	// Each line is a field, in the format NAME=VALUE.  We assume the field
	// name doesn't contain an "=".  Lines without an "=" are currently
	// just ignored.
	while (true) {
	    string::size_type old_i = i;
	    i = text.find('\n', i);
	    string line = text.substr(old_i, i - old_i);
	    string::size_type j = line.find('=');
	    if (j != string::npos) {
		string key = line.substr(0, j);
		string value = field[key];
		if (!value.empty()) value += '\t';
		value += line.substr(j + 1);
		field[key] = value;
	    }
	    if (i == string::npos) break;
	    ++i;
	}
    }

    return eval(fmt, param);
}

void
parse_omegascript()
{
    try {
	std::string output = eval_file(fmtname);
	if (!set_content_type) {
	    cout << "Content-Type: text/html" << std::endl;
	}
	cout << std::endl;
	cout << output;
    } catch (...) {
	// Ensure the headers have been output so that any exception gets
	// reported rather than giving a server error.
	if (!set_content_type) {
	    cout << "Content-Type: text/html" << std::endl;
	}
	cout << std::endl;
	throw;
    }
}

static void
ensure_query_parsed()
{
    if (query_parsed) return;
    query_parsed = true;

    MCI val;
    pair<MCI, MCI> g;

    // Should we discard the existing R-set recorded in R CGI parameters?
    bool discard_rset = true;

    // Should we force the first page of hits (and ignore [ > < # and TOPDOC
    // CGI parameters)?
    bool force_first_page = true;

    string v;
    // get list of terms from previous iteration of query
    val = cgi_params.find("xP");
    if (val == cgi_params.end()) val = cgi_params.find("OLDP");
    if (val != cgi_params.end()) {
	v = val->second;
    } else {
	// if xP not given, default to keeping the rset and don't force page 1
	discard_rset = false;
	force_first_page = false;
    }
    querytype result = set_probabilistic(v);
    switch (result) {
	case BAD_QUERY:
	    break;
	case NEW_QUERY:
	    break;
	case SAME_QUERY:
        case EXTENDED_QUERY:
	    // If we've changed database, force the first page of hits
	    // and discard the R-set (since the docids will have changed)
	    val = cgi_params.find("xDB");
	    if (val != cgi_params.end() && val->second != dbname) break;
	    if (result == SAME_QUERY && force_first_page) {
		force_first_page = false;
		val = cgi_params.find("xFILTERS");
		string xfilters;
		if (val != cgi_params.end()) xfilters = val->second;
		if (filters != xfilters) {
		    // Filters changed since last query
		    force_first_page = true;
		}
	    }
	    discard_rset = false;
	    break;
    }

    if (!force_first_page) {
	// Work out which mset element is the first hit we want
	// to display
	val = cgi_params.find("TOPDOC");
	if (val != cgi_params.end()) {
	    topdoc = atol(val->second.c_str());
	}

	// Handle next, previous, and page links
	if (cgi_params.find(">") != cgi_params.end()) {
	    topdoc += hits_per_page;
	} else if (cgi_params.find("<") != cgi_params.end()) {
	    if (topdoc >= hits_per_page)
		topdoc -= hits_per_page;
	    else
		topdoc = 0;
	} else if ((val = cgi_params.find("[")) != cgi_params.end() ||
		   (val = cgi_params.find("#")) != cgi_params.end()) {
	    topdoc = (atol(val->second.c_str()) - 1) * hits_per_page;
	}

	// raw_search means don't snap TOPDOC to a multiple of HITSPERPAGE.
	// Normally we snap TOPDOC like this so that things work nicely if
	// HITSPERPAGE is in a <select> or on radio buttons.  If we're
	// postprocessing the output of omega and want variable sized pages,
	// this is unhelpful.
	bool raw_search = false;
	val = cgi_params.find("RAWSEARCH");
	if (val != cgi_params.end()) {
	    raw_search = bool(atol(val->second.c_str()));
	}

	if (!raw_search) topdoc = (topdoc / hits_per_page) * hits_per_page;
    }

    if (!discard_rset) {
	// put documents marked as relevant into the rset
	g = cgi_params.equal_range("R");
	for (MCI i = g.first; i != g.second; i++) {
	    const string & value = i->second;
	    for (size_t j = 0; j < value.size(); j = value.find('.', j)) {
		while (value[j] == '.') ++j;
		Xapian::docid d = atoi(value.c_str() + j);
		if (d) {
		    rset.add_document(d);
		    ticked[d] = true;
		}
	    }
	}
    }
}

// run query if we haven't already
static void
ensure_match()
{
    if (done_query) return;

    run_query();
    done_query = true;
    last = mset.get_matches_lower_bound();
    if (last == 0) {
	// Otherwise topdoc ends up being -6 if it's non-zero!
	topdoc = 0;
    } else {
	if (topdoc >= last)
	    topdoc = ((last - 1) / hits_per_page) * hits_per_page;
	// last is the count of documents up to the end of the current page
	// (as returned by $last)
	if (topdoc + hits_per_page < last)
	    last = topdoc + hits_per_page;
    }
}
