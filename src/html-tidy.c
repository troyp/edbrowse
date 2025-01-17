/*********************************************************************
html-tidy.c: use tidy5 to parse html tags.
This is the only file that includes tidy.h, or accesses the tidy library.
If you prefer a different parser in the future,
write another file, html-foo.c, having the same connection to edbrowse,
and change the makefile accordingly.
If tidy5 changes its API, you only need edit this file.
Note that tidy has Bool yes and no, which, fortunately, do not collide with
edbrowse bool true false.
*********************************************************************/

#include "eb.h"

#include <tidy.h>
#include <tidybuffio.h>

/*********************************************************************
This routine preprocesses the html text to work around
any shortcomings of tidy that are not worth fixing just for edbrowse,
or have not been fixed yet.
Other shortcomings are easier to manage after the fact,
dealing with the tree of nodes - see nestedAnchors() in decorate.c for example.
But some things must be managed prior to tidy parse.
Return null if there is no need to change the text.
Otherwise return an allocated string.
The first workaround here is the expansion of < > inside a textarea.
The second is changing tag optgroup to zptgroup, cause if <optgroup> is not
properly nested, tidy really jumps the tracks.
The third is changing <code> to <zode> since <pre><code> blocks lose the line
breaks, and we don't do anything with <code> anyways.
These are blind substitutions, and I don't like them.
If <code> appears in a string, or as part of javascript, or json, I'm up the creek!
I could try to parse the html at some level, to know they are tags,
but at that point I'm practically rewriting tidy.
*********************************************************************/

char *tidyPreprocess(const char *h)
{
	char *ns = 0;		/* the new string */
 bool first = false;
	int l;
	char *inside, *expanded, *u;
	const char *lg, *s, *h0;

	if(strcasestr(h, "<optgroup")) {
// but maybe it's nested properly.
		s = h;
		while (true) {
			const char *a = strcasestr(s, "<optgroup");
			const char *b = strcasestr(s, "/optgroup>");
			if(!a && !b)
				goto optgroup_ok;
			if(!a || !b || b < a)
				break;
			a = strcasestr(a + 9, "<optgroup");
			if(a && a < b)
				break;
			s = b + 10;
		}
// something not balanced properly, have to muck
		u = ns = cloneString(h);
		while((u = strcasestr(u, "<optgroup")))
			u[1] = 'z';
		u = ns;
		while((u = strcasestr(u, "/optgroup>")))
			u[1] = 'z';
		first = true;
		h = ns;
	}
optgroup_ok:

	if(strcasestr(h, "<code>")) {
		u = ns = cloneString(h);
		while((u = strcasestr(u, "<code>")))
			u[1] = 'z';
		u = ns;
		while((u = strcasestr(u, "/code>")))
			u[1] = 'z';
		if(first)
			cnzFree(h);
		first = true;
		h = ns;
	}

	s = strcasestr(h, "<textarea");
	if (!s)
		return ns;

	h0 = h;
	ns = initString(&l);
	stringAndBytes(&ns, &l, h, s - h);
	h = s;
	while (true) {
/* next textarea */
		s = strcasestr(h, "<textarea");
		if (!s)
			break;
		s = strchr(s, '>');
		if (!s)
			break;
		++s;
		stringAndBytes(&ns, &l, h, s - h);
		h = s;
		s = strcasestr(h, "</textarea");
		if (!s)
			break;
		lg = strpbrk(h, "<>");
/* lg is at most s */
		if (lg == s)
			continue;
		inside = pullString1(h, s);
		expanded = htmlEscapeTextarea(inside);
		stringAndString(&ns, &l, expanded);
		nzFree(inside);
		nzFree(expanded);
		h = s;
	}
	stringAndString(&ns, &l, h);
	if(first)
		cnzFree(h0);
	return ns;
}

/* the tidy structure corresponding to the html */
static TidyDoc tdoc;

/* traverse the tidy tree with a callback function */
typedef void (*tidyNodeCallback) (TidyNode node, int level, bool opentag);
static tidyNodeCallback traverse_tidycall;
/* possible callback functions */
static void printNode(TidyNode node, int level, bool opentag);
static void convertNode(TidyNode node, int level, bool opentag);

static void traverseNode(TidyNode node, int level)
{
	TidyNode child;

/* first the callback function */
	(*traverse_tidycall) (node, level, true);

/* and now the children */
	for (child = tidyGetChild(node); child; child = tidyGetNext(child))
		traverseNode(child, level + 1);

	(*traverse_tidycall) (node, level, false);
}

static void traverseTidy(void)
{
	traverseNode(tidyGetRoot(tdoc), 0);
}

/* This is like the default tidy error reporter, except messages are suppressed
 * unless debugLevel >= 3, and in that case they are sent to stdout
 * rather than stderr, like most edbrowse messages.
 * Since these are debug messages, they are not internationalized. */

static Bool TIDY_CALL tidyErrorHandler(TidyDoc tdoc, TidyReportLevel lvl,
				       uint line, uint col, ctmbstr mssg)
{
	if (debugLevel >= 4 && lvl != TidyInfo)
		debugPrint(4, "%s", mssg);
	return no;
}

/* the entry point */
void html2nodes(const char *htmltext, bool startpage)
{
	char *htmlfix = 0;

	tdoc = tidyCreate();
	if (!startpage)
		tidyOptSetInt(tdoc, TidyBodyOnly, yes);
	tidySetReportFilter(tdoc, tidyErrorHandler);
//    tidySetReportFilter(tdoc, tidyReportFilter);

	// the following tidyOptSetBool implements 
	// a fix for https://github.com/htacg/tidy-html5/issues/348 
	tidyOptSetBool(tdoc, TidyEscapeScripts, no);
	tidyOptSetBool(tdoc, TidyDropEmptyElems, no);
	tidyOptSetBool(tdoc, TidyDropEmptyParas, no);
	tidyOptSetBool(tdoc, TidyLiteralAttribs, yes);
	tidyOptSetBool(tdoc, TidyStyleTags, no);

	tidySetCharEncoding(tdoc, (cons_utf8 ? "utf8" : "latin1"));

	htmlfix = tidyPreprocess(htmltext);
	if (htmlfix) {
		tidyParseString(tdoc, htmlfix);
		nzFree(htmlfix);
	} else
		tidyParseString(tdoc, htmltext);

	tidyCleanAndRepair(tdoc);

	if (debugLevel >= 5) {
		traverse_tidycall = printNode;
		traverseTidy();
	}

/* convert tidy nodes into edbrowse nodes */
	traverse_tidycall = convertNode;
	traverseTidy();

	tidyRelease(tdoc);
}

/* this is strictly for debugging, level >= 5 */
static void printNode(TidyNode node, int level, bool opentag)
{
	ctmbstr name;
	TidyAttr tattr;
	FILE *f;

	f = debugFile ? debugFile : stdout;
	if (!opentag) {
		fprintf(f, "}\n");
		return;
	}

	switch (tidyNodeGetType(node)) {
	case TidyNode_Root:
		name = "Root";
		break;
	case TidyNode_DocType:
		name = "DOCTYPE";
		break;
	case TidyNode_Comment:
		name = "Comment";
		break;
	case TidyNode_ProcIns:
		name = "Processing Instruction";
		break;
	case TidyNode_Text:
		name = "Text";
		break;
	case TidyNode_CDATA:
		name = "CDATA";
		break;
	case TidyNode_Section:
		name = "XML Section";
		break;
	case TidyNode_Asp:
		name = "ASP";
		break;
	case TidyNode_Jste:
		name = "JSTE";
		break;
	case TidyNode_Php:
		name = "PHP";
		break;
	case TidyNode_XmlDecl:
		name = "XML Declaration";
		break;
	case TidyNode_Start:
	case TidyNode_End:
	case TidyNode_StartEnd:
	default:
		name = tidyNodeGetName(node);
		break;
	}
	assert(name != NULL);
	fprintf(f, "Node(%d): %s {\n", level, ((char *)name));
/* the ifs could be combined with && */
	if (stringEqual(((char *)name), "Text")) {
		TidyBuffer tnv = { 0 };	/* text-node value */
		tidyBufClear(&tnv);
		tidyNodeGetValue(tdoc, node, &tnv);
		fprintf(f, "Text: %s\n", tnv.bp);
		if (tnv.size)
			tidyBufFree(&tnv);
	}

/* Get the first attribute for the node */
	tattr = tidyAttrFirst(node);
	while (tattr != NULL) {
/* Print the node and its attribute */
		fprintf(f, "@%s = %s\n", tidyAttrName(tattr),
			tidyAttrValue(tattr));
/* Get the next attribute */
		tattr = tidyAttrNext(tattr);
	}
}

/* remove tags from start and end of a string, for innerHTML */
static void tagStrip(char *line)
{
	char *s = line;
	if (*s != '<') {
/* this shouldn't happen, don't know what to do. */
		return;
	}

	s = strchr(line, '>');
	if (!s)
		return;
	++s;
	skipWhite2(&s);
	strmove(line, s);
	trimWhite(line);
	s = line + strlen(line);
	if (s == line || s[-1] != '>')
		return;
/* back up over </foo> */
	--s;
	while (s >= line) {
		if (*s == '<') {
			*s = 0;
			return;
		}
		--s;
	}
}

static void convertNode(TidyNode node, int level, bool opentag)
{
	ctmbstr name;
	TidyAttr tattr;
	Tag *t;
	int nattr;		/* number of attributes */
	int i;


	switch (tidyNodeGetType(node)) {
	case TidyNode_Text:
		name = "text";
		break;
	case TidyNode_DocType:
		name = "doctype";
		break;
	case TidyNode_Start:
	case TidyNode_End:
	case TidyNode_StartEnd:
		name = tidyNodeGetName(node);
		break;
	default:
		return;
	}

	t = newTag(cf, (char *)name);
	if (!t)
		return;

	if (!opentag) {
		t->slash = true;
		return;
	}

/* if a js script, remember the line number for error messages */
	if (t->action == TAGACT_SCRIPT)
		t->js_ln = tidyNodeLine(node);

/* this is the open tag, set the attributes */
/* special case for text tag */
	if (t->action == TAGACT_TEXT) {
		TidyBuffer tnv = { 0 };	/* text-node value */
		tidyBufClear(&tnv);
		tidyNodeGetValue(tdoc, node, &tnv);
		if (tnv.size) {
// check here for &#0; or &#2; both of those are bad!
			for (i = 0; (unsigned)i < tnv.size; ++i)
				if (tnv.bp[i] == 0
				    || tnv.bp[i] == InternalCodeChar)
					tnv.bp[i] = InternalCodeCharAlternate;
			t->textval = cloneString((char *)tnv.bp);
			tidyBufFree(&tnv);
		}
	}

	nattr = 0;
	tattr = tidyAttrFirst(node);
	while (tattr != NULL) {
		++nattr;
		tattr = tidyAttrNext(tattr);
	}

	t->attributes = allocMem(sizeof(char *) * (nattr + 1));
	t->atvals = allocMem(sizeof(char *) * (nattr + 1));
	i = 0;
	tattr = tidyAttrFirst(node);
	while (tattr != NULL) {
		t->attributes[i] = cloneString(tidyAttrName(tattr));
		t->atvals[i] = cloneString(tidyAttrValue(tattr));
		if (t->atvals[i] == NULL)
			t->atvals[i] = emptyString;
		++i;
		tattr = tidyAttrNext(tattr);
	}
	t->attributes[i] = 0;
	t->atvals[i] = 0;

/* innerHTML, only for certain tags */
	if (t->info->bits & TAG_INNERHTML) {
		TidyBuffer tnv = { 0 };	/* text-node value */
		tidyBufClear(&tnv);
		t->innerHTML = emptyString;
		tidyNodeGetText(tdoc, node, &tnv);
		if (tnv.size) {
/* But it's not the original html, it has been sanitized.
 * Warning! Memory consumed could, theoretically,
 * grow as the size of the document squared. */
			t->innerHTML = cloneString((char *)tnv.bp);
			tagStrip(t->innerHTML);
			tidyBufFree(&tnv);
		}
	}

}
