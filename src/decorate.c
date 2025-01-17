/*********************************************************************
decorate.c:
sanitize a tree of nodes produced by html,
and decorate the tree with the corresponding js objects.
A <form> tag has a corresponding Form object in the js world, etc.
This is done for the html that is on the initial web page,
and any html that is produced by javascript via
foo.innerHTML = string or document.write(string).
*********************************************************************/

#include "eb.h"

/* The current (foreground) edbrowse window and frame.
 * These are replaced with stubs when run within the javascript process. */
Window *cw;
Frame *cf;
int gfsn;

/* traverse the tree of nodes with a callback function */
nodeFunction traverse_callback;

/* possible callback functions in this file */
static void prerenderNode(Tag *node, bool opentag);
static void jsNode(Tag *node, bool opentag);
static void pushAttributes(const Tag *t);

static bool treeOverflow;

static void traverseNode(Tag *node)
{
	Tag *child;

	if (node->visited) {
		treeOverflow = true;
		debugPrint(4, "node revisit %s %d", node->info->name,
			   node->seqno);
		return;
	}
	node->visited = true;

	(*traverse_callback) (node, true);
	for (child = node->firstchild; child; child = child->sibling)
		traverseNode(child);
	(*traverse_callback) (node, false);
}

void traverseAll(int start)
{
	Tag *t;
	int i;

	treeOverflow = false;
	for (i = start; i < cw->numTags; ++i) {
		t = tagList[i];
		t->visited = false;
	}

	for (i = start; i < cw->numTags; ++i) {
		t = tagList[i];
		if (!t->parent && !t->slash && !t->dead) {
			debugPrint(6, "traverse start at %s %d", t->info->name, t->seqno);
			traverseNode(t);
		}
	}

	if (treeOverflow)
		debugPrint(3, "malformed tree!");
}

static int nopt;		/* number of options */
/* None of these tags nest, so it is reasonable to talk about
 * the current open tag. */
static Tag *currentForm, *currentSel, *currentOpt, *currentStyle;
static const char *optg; // option group
static Tag *currentTitle, *currentScript, *currentTA;
static Tag *currentA;
static char *radioCheck;
static int radio_l;

const char *attribVal(const Tag *t, const char *name)
{
	const char *v;
	int j;
	if (!t->attributes)
		return 0;
	j = stringInListCI(t->attributes, name);
	if (j < 0)
		return 0;
	v = t->atvals[j];
	return v;
}

bool attribPresent(const Tag *t, const char *name)
{
	int j = stringInListCI(t->attributes, name);
	return (j >= 0);
}

// Push an attribute onto an html tag.
// Value is already allocated, name is not.
// So far used by domSetsLinkage and linkPipe.
void setTagAttr(Tag *t, const char *name, char *val)
{
	int nattr = 0;		/* number of attributes */
	int i = -1;
	if (!val)
		return;
	if (t->attributes) {
		for (nattr = 0; t->attributes[nattr]; ++nattr)
			if (stringEqualCI(name, t->attributes[nattr]))
				i = nattr;
	}
	if (i >= 0) {
		cnzFree(t->atvals[i]);
		t->atvals[i] = val;
		return;
	}
/* push */
	if (!nattr) {
		t->attributes = allocMem(sizeof(char *) * 2);
		t->atvals = allocMem(sizeof(char *) * 2);
	} else {
		t->attributes =
		    reallocMem(t->attributes, sizeof(char *) * (nattr + 2));
		t->atvals = reallocMem(t->atvals, sizeof(char *) * (nattr + 2));
	}
	t->attributes[nattr] = cloneString(name);
	t->atvals[nattr] = val;
	++nattr;
	t->attributes[nattr] = 0;
	t->atvals[nattr] = 0;
}

static void linkinTree(Tag *parent, Tag *child)
{
	Tag *c, *d;
	child->parent = parent;

	if (!parent->firstchild) {
		parent->firstchild = child;
		return;
	}

	for (c = parent->firstchild; c; c = c->sibling) {
		d = c;
	}
	d->sibling = child;
}

static void makeButton(void)
{
	Tag *t = newTag(cf, "input");
	t->controller = currentForm;
	t->itype = INP_SUBMIT;
	t->value = emptyString;
	t->step = 1;
	linkinTree(currentForm, t);
}

Tag *findOpenTag(Tag *t, int action)
{
	int count = 0;
	while ((t = t->parent)) {
		if (t->action == action)
			return t;
		if (++count == 10000) {	// tree shouldn't be this deep
			debugPrint(1, "infinite loop in findOpenTag()");
			break;
		}
	}
	return 0;
}

static Tag *findOpenSection(Tag *t)
{
	int count = 0;
	while ((t = t->parent)) {
		if (t->action == TAGACT_TBODY || t->action == TAGACT_THEAD ||
		    t->action == TAGACT_TFOOT)
			return t;
		if (++count == 10000) {	// tree shouldn't be this deep
			debugPrint(1, "infinite loop in findOpenTag()");
			break;
		}
	}
	return 0;
}

Tag *findOpenList(Tag *t)
{
	while ((t = t->parent))
		if (t->action == TAGACT_OL || t->action == TAGACT_UL)
			return t;
	return 0;
}

/*********************************************************************
tidy workaround functions.
Consider html like this.
<body>
<A href=http://www.edbrowse.org>Link1
<A href=http://www.edbrowse.org>Link2
<A href=http://www.edbrowse.org>Link3
</body>
Each anchor should close the one before, thus rendering as
 {Link1} {Link2} {Link3}
But tidy does not do this; it allows anchors to nest, thus
 {Link1{Link2{Link3}}}
Not a serious problem really, it just looks funny.
And yes, html like this does appear in the wild.
This routine restructures the tree to move the inner anchor
back up to the same level as the outer anchor.
*********************************************************************/

static void nestedAnchors(int start)
{
	Tag *a1, *a2, *p, *c;
	int j;

	for (j = start; j < cw->numTags; ++j) {
		a2 = tagList[j];
		if (a2->action != TAGACT_A)
			continue;
		a1 = findOpenTag(a2, TAGACT_A);
		if (!a1)
			continue;

/* delete a2 from the tree */
		p = a2->parent;
		a2->parent = 0;
		if (p->firstchild == a2)
			p->firstchild = a2->sibling;
		else {
			c = p->firstchild;
			while (c->sibling) {
				if (c->sibling == a2) {
					c->sibling = a2->sibling;
					break;
				}
				c = c->sibling;
			}
		}
		a2->sibling = 0;

/* then link a2 up next to a1 */
		a2->parent = a1->parent;
		a2->sibling = a1->sibling;
		a1->sibling = a2;
	}
}

/*********************************************************************
Tables are suppose to have bodies, I guess.
So <table><tr> becomes <table><tbody><tr>
Find each table and look at its children.
Note the tags between sections, where section is tHead, tBody, or tFoot.
If that span includes <tr>, then put those tags under a new tBody.
*********************************************************************/

static void insert_tbody1(Tag *s1, Tag *s2,
			  Tag *tbl);
static bool tagBelow(Tag *t, int action);

static void insert_tbody(int start)
{
	int i, end = cw->numTags;
	Tag *tbl, *s1, *s2;

	for (i = start; i < end; ++i) {
		tbl = tagList[i];
		if (tbl->action != TAGACT_TABLE)
			continue;
		s1 = 0;
		do {
			s2 = (s1 ? s1->sibling : tbl->firstchild);
			while (s2 && s2->action != TAGACT_TBODY
			       && s2->action != TAGACT_THEAD
			       && s2->action != TAGACT_TFOOT)
				s2 = s2->sibling;
			insert_tbody1(s1, s2, tbl);
			s1 = s2;
		} while (s1);
	}
}

static void insert_tbody1(Tag *s1, Tag *s2,
			  Tag *tbl)
{
	Tag *s1a = (s1 ? s1->sibling : tbl->firstchild);
	Tag *u, *uprev, *ns;	// new section

	if (s1a == s2)		// nothing between
		return;

// Look for the direct html <table><tr><th>.
// If th is anywhere else down the path, we won't find it.
	if (!s1 && s1a->action == TAGACT_TR &&
	    (u = s1a->firstchild) && stringEqual(u->info->name, "th")) {
		ns = newTag(cf, "thead");
		tbl->firstchild = ns;
		ns->parent = tbl;
		ns->firstchild = s1a;
		s1a->parent = ns;
		ns->sibling = s1a->sibling;
		s1a->sibling = 0;
		s1 = ns;
		s1a = s1->sibling;
	}

	for (u = s1a; u != s2; u = u->sibling)
		if (tagBelow(u, TAGACT_TR))
			break;
	if (u == s2)		// no rows below
		return;

	ns = newTag(cf, "tbody");
	for (u = s1a; u != s2; u = u->sibling)
		uprev = u, u->parent = ns;
	if (s1)
		s1->sibling = ns;
	else
		tbl->firstchild = ns;
	if (s2)
		uprev->sibling = 0, ns->sibling = s2;
	ns->firstchild = s1a;
	ns->parent = tbl;
}

/*********************************************************************
Bad html will derail tidy, so that <a><div>stuff</div></a>
will push div outside the anchor, to render as  {} stuff
m.facebook.com is loaded with them.
Here is a tiny example.

<body>
<input type=button name=whatever value=hohaa>
<a href="#bottom"><div>Cognitive business is here</div></a>
</body>

This routine puts it back.
An anchor with no children followd by div
moves div under the anchor.
For a while I had this function commented out, like it caused a problem,
but I can't see why or how, so it's back, and facebook looks better.

As an after kludge, don't move <div> under <a> if <div> has an anchor beneath it.
That could create nested anchors, which we already worked hard to get rid of.   Eeeeeeesh.

This and other tidy workaround functions are based on heuristics,
and suffer from false positives and false negatives,
the former being the more serious problem -
i.e. we rearrange the tree when we shouldn't.
Even when we do the right thing, there is another problem,
innerHTML is wrong, and doesn't match the tree of nodes
or the original source.
innerHTML comes to us from tidy, after it has fixed (sometimes broken) things.
Add <script> to the above, browse, jdb, and look at document.body.innerHTML.
It does not match the source, in fact it represents the tree *before* we fixed it.
There really isn't anything I can do about that.
In so many ways, the better approach is to fix tidy, but sometimes that is out of our hands.
*********************************************************************/

static bool tagBelow(Tag *t, int action)
{
	Tag *c;

	if (t->action == action)
		return true;
	for (c = t->firstchild; c; c = c->sibling)
		if (tagBelow(c, action))
			return true;
	return false;
}

static void emptyAnchors(int start)
{
	int j;
	Tag *a0, *div, *up;

	for (j = start; j < cw->numTags; ++j) {
		a0 = tagList[j];
		if (a0->action != TAGACT_A || a0->firstchild)
			continue;
// anchor no children
		for (up = a0; up; up = up->parent)
			if (up->sibling)
				break;
		if (!up || !(div = up->sibling) || div->action != TAGACT_DIV)
			continue;
// div follows
/* would moving this create nested anchors? */
		if (tagBelow(div, TAGACT_A))
			continue;
/* shouldn't have inputs or forms in an anchor. */
		if (tagBelow(div, TAGACT_INPUT))
			continue;
		if (tagBelow(div, TAGACT_FORM))
			continue;
		up->sibling = div->sibling;
		a0->firstchild = div;
		div->parent = a0;
		div->sibling = 0;
	}
}

/*********************************************************************
If a form is in a table, but not in tr or td, it closes immediately,
and all the following inputs are orphaned.
Check for an empty form beneath table, and move all the following siblings
down into the form.
*********************************************************************/

static void tableForm(int start)
{
	int j;
	Tag *form, *table, *t;

	for (j = start; j < cw->numTags; ++j) {
		form = tagList[j];
		if (form->action != TAGACT_FORM || form->firstchild)
			continue;
		t = form;
		for (table = form->sibling; table; table = table->sibling) {
			if (table->action == TAGACT_TABLE &&
			    tagBelow(table, TAGACT_INPUT)) {
/* table with inputs below; move it to form */
/* hope this doesn't break anything */
				table->parent = form;
				form->firstchild = table;
				t->sibling = table->sibling;
				table->sibling = 0;
				break;
			}
			t = table;
		}
	}
}

/*********************************************************************
<link rel=stylesheet href=http://example.com/|this.css,that.css,foo.css>
This is shortcut for three css files.
It is rarely used, but a long example can be found in https://www.amazon.com/
It may not be the right approach, but the easiest is to spin it off into
three separate link tags with the appropriate urls.
*********************************************************************/

static void linkPipe(int start)
{
	int j, n, last = cw->numTags;
	char *mark, *url, *follow, *a, *b;
	const char *rel, *type;
	Tag *l, *e;

	for (j = start; j < last; ++j) {
		l = tagList[j];
		if (l->action != TAGACT_LINK)
			continue;
		if(!l->href || !isURL(l->href) ||
		!(mark = strchr(l->href, '|')))
			continue;
		rel = attribVal(l, "rel");
		type = attribVal(l, "type");
		if (!stringEqualCI(type, "text/css") &&
		    !stringEqualCI(rel, "stylesheet"))
// not a stylesheet, it doesn't matter
			continue;
		e = l->sibling;
		n = mark - l->href;
		url = allocMem(n + 1);
		strncpy(url, l->href, n);
		url[n] = 0;
		a = follow = cloneString(mark+1);
		while(*a) {
			char *resolve;
			if((b = strchr(a, ',')))
				*b = 0;
			resolve = resolveURL(url, a);
			if(a == follow) {
// first one, just displace the href url
				nzFree(l->href);
				l->href = resolve;
			} else {
				Tag *t = newTag(cf, "link");
				t->href = resolve;
				if(rel)
					setTagAttr(t, "rel", cloneString(rel));
				if(type)
					setTagAttr(t, "type", cloneString(type));
				t->parent = l->parent;
				t->sibling = e;
				l->sibling = t;
				l = t;
			}
			if(!b)
				break;
			a = b + 1;
		}
		free(url);
		free(follow);
	}
}

void formControl(Tag *t, bool namecheck)
{
	int itype = t->itype;
	char *myname = (t->name ? t->name : t->id);
	Tag *cform = currentForm;
	if (!cform) {
/* nodes could be created dynamically, not through html */
		cform = findOpenTag(t, TAGACT_FORM);
	}
	if (cform)
		t->controller = cform;
	else if (itype != INP_BUTTON && itype != INP_SUBMIT && !htmlGenerated)
		debugPrint(3, "%s is not part of a fill-out form",
			   t->info->desc);
	if (namecheck && !myname && !htmlGenerated)
		debugPrint(3, "%s does not have a name", t->info->desc);
}

const char *const inp_types[] = {
	"reset", "button", "image", "submit",
	"hidden", "text", "file",
	"select", "textarea", "radio", "checkbox",
	0
};

/*********************************************************************
Here are some other input types that should have additional syntax checks
performed on them, but as far as this version of edbrowse is concerned,
they are equivalent to text. Just here to suppress warnings.
List taken from https://www.tutorialspoint.com/html/html_input_tag.htm
*********************************************************************/

const char *const inp_others[] = {
	"no_minor", "date", "datetime", "datetime-local",
	"month", "week", "time", "email", "range",
	"search", "tel", "url", "number", "password",
	0
};

/* helper function for input tag */
void htmlInputHelper(Tag *t)
{
	int n = INP_TEXT;
	int len;
	char *myname = (t->name ? t->name : t->id);
	const char *s = attribVal(t, "type");
	bool isbutton = stringEqual(t->info->name, "button");

	t->itype = (isbutton ? INP_BUTTON : INP_TEXT);
	if (s && *s) {
		n = stringInListCI(inp_types, s);
		if (n < 0) {
			n = stringInListCI(inp_others, s);
			if (n < 0)
				debugPrint(3, "unrecognized input type %s", s);
			else
				t->itype = INP_TEXT, t->itype_minor = n;
			if (n == INP_PW)
				t->masked = true;
		} else
			t->itype = n;
	}
// button no type means submit
	if (!s && isbutton)
		t->itype = INP_SUBMIT;

	s = attribVal(t, "maxlength");
	len = 0;
	if (s)
		len = stringIsNum(s);
	if (len > 0)
		t->lic = len;

// No preset value on file, for security reasons.
// <input type=file value=/etc/passwd> then submit via onload().
	if (n == INP_FILE) {
		nzFree(t->value);
		t->value = 0;
		cnzFree(t->rvalue);
		t->rvalue = 0;
	}

/* In this case an empty value should be "", not null */
	if (t->value == 0)
		t->value = emptyString;
	if (t->rvalue == 0)
		t->rvalue = cloneString(t->value);

	if (n == INP_RADIO && t->checked && radioCheck && myname) {
		char namebuf[200];
		if (strlen(myname) < sizeof(namebuf) - 3) {
			if (!*radioCheck)
				stringAndChar(&radioCheck, &radio_l, '|');
			sprintf(namebuf, "|%s|", t->name);
			if (strstr(radioCheck, namebuf)) {
				debugPrint(3,
					   "multiple radio buttons have been selected");
				return;
			}
			stringAndString(&radioCheck, &radio_l, namebuf + 1);
		}
	}

	/* Even the submit fields can have a name, but they don't have to */
	formControl(t, (n > INP_SUBMIT));
}

/* return an allocated string containing the text entries for the checked options */
char *displayOptions(const Tag *sel)
{
	const Tag *t;
	char *opt;
	int opt_l;

	opt = initString(&opt_l);
	for (t = cw->optlist; t; t = t->same) {
		if (t->controller != sel)
			continue;
		if (!t->checked)
			continue;
		if (*opt)
			stringAndChar(&opt, &opt_l, selsep);
		stringAndString(&opt, &opt_l, t->textval);
	}

	return opt;
}

static void prerenderNode(Tag *t, bool opentag)
{
	int itype;		/* input type */
	int j;
	int action = t->action;
	const char *a;		/* usually an attribute */

	if (t->step >= 1)
		return;
	debugPrint(6, "prend %c%s %d",
		   (opentag ? ' ' : '/'), t->info->name,
		   t->seqno);
	if (!opentag)
		t->step = 1;

	switch (action) {
	case TAGACT_NOSCRIPT:
// If javascript is enabled kill everything under noscript
		if (isJSAlive && !opentag)
			underKill(t);
		break;

	case TAGACT_TEXT:
		if (!opentag || !t->textval)
			break;

		if (currentTitle) {
			if (!cw->htmltitle) {
				cw->htmltitle = cloneString(t->textval);
				spaceCrunch(cw->htmltitle, true, false);
			}
			t->deleted = true;
			break;
		}

		if (currentOpt) {
			currentOpt->textval = cloneString(t->textval);
			spaceCrunch(currentOpt->textval, true, false);
			t->deleted = true;
			break;
		}

		if (currentStyle) {
			t->deleted = true;
			break;
		}

		if (currentScript) {
			currentScript->textval = cloneString(t->textval);
			t->deleted = true;
			break;
		}

		if (currentTA) {
			currentTA->value = cloneString(t->textval);
/*********************************************************************
Warning - tidy lops off any whitespace between the text message and </textarea>
We simply don't see it and have no way to know.
I'm gonna leave it be, as though it runs together.
<textarea>prepared message</textarea>
Often times it is blank, <textarea></textarea>, and probably should
remain so. jquery and other libraries crank out these blank textareas,
so you'll see them around.
Although we're right most of the time, sometimes we'll be wrong,
as when <textarea> is on the next line,
and there should be \n at the end of the message.
Oh well - best we can do.
*********************************************************************/
			leftClipString(currentTA->value);
			currentTA->rvalue = cloneString(currentTA->value);
			t->deleted = true;
			break;
		}

/* text is on the page */
		if (currentA) {
			char *s;
			for (s = t->textval; *s; ++s)
				if (isalnumByte(*s)) {
					currentA->textin = true;
					break;
				}
		}
		break;

	case TAGACT_TITLE:
		if((t->controller = findOpenTag(t, TAGACT_SVG))) {
// this is title for svg, not for the document.
// Just turn things into span.
			t->action = t->controller->action = TAGACT_SPAN;
			break;
		}
		currentTitle = (opentag ? t : 0);
		break;

	case TAGACT_SCRIPT:
		currentScript = (opentag ? t : 0);
		break;

	case TAGACT_A:
		currentA = (opentag ? t : 0);
		break;

	case TAGACT_FORM:
		if (opentag) {
			currentForm = t;
			a = attribVal(t, "method");
			if (a) {
				if (stringEqualCI(a, "post"))
					t->post = true;
				else if (!stringEqualCI(a, "get"))
					debugPrint(3,
						   "form method should be get or post");
			}
			a = attribVal(t, "enctype");
			if (a) {
				if (stringEqualCI(a, "multipart/form-data"))
					t->mime = true;
				else if (stringEqualCI(a, "text/plain"))
					t->plain = true;
				else if (!stringEqualCI(a, 
					  "application/x-www-form-urlencoded"))
					debugPrint(3,
						   "unrecognized enctype, plese use multipart/form-data or application/x-www-form-urlencoded or text/plain");
			}
			if ((a = t->href)) {
				const char *prot = getProtURL(a);
				if (prot) {
					if (stringEqualCI(prot, "mailto"))
						t->bymail = true;
					else if (stringEqualCI
						 (prot, "javascript"))
						t->javapost = true;
					else if (stringEqualCI(prot, "https"))
						t->secure = true;
					else if (!stringEqualCI(prot, "http") &&
						 !stringEqualCI(prot, "gopher"))
						debugPrint(3,
							   "form cannot submit using protocol %s",
							   prot);
				}
			}

			nzFree(radioCheck);
			radioCheck = initString(&radio_l);
		}
		if (!opentag && currentForm) {
			if (t->ninp && !t->submitted) {
				makeButton();
				t->submitted = true;
			}
			currentForm = 0;
		}
		break;

	case TAGACT_INPUT:
		if (!opentag)
			break;
		htmlInputHelper(t);
		itype = t->itype;
		if (itype == INP_HIDDEN)
			break;
		if (currentForm) {
			++currentForm->ninp;
			if (itype == INP_SUBMIT || itype == INP_IMAGE)
				currentForm->submitted = true;
			if (itype == INP_BUTTON && t->onclick)
				currentForm->submitted = true;
			if (itype > INP_HIDDEN && itype <= INP_SELECT
			    && t->onchange)
				currentForm->submitted = true;
		}
		break;

	case TAGACT_OPTG:
		if(opentag)
			optg = attribVal(t, "label");
		else
			optg = 0;
		break;

	case TAGACT_OPTION:
		if (!opentag) {
			currentOpt = 0;
			optg = 0;
// in datalist, the value becomes the text
			if(currentSel && currentSel->action == TAGACT_DATAL) {
				nzFree(t->textval);
				t->textval = cloneString(t->value);
			}
			a = attribVal(t, "label");
			if(a && *a) {
				nzFree(t->textval);
				t->textval = cloneString(a);
			}
			break;
		}
		if (!currentSel) {
			debugPrint(3,
				   "option appears outside a select statement");
			optg = 0;
			break;
		}
		currentOpt = t;
		t->controller = currentSel;
		t->lic = nopt++;
		if (attribPresent(t, "selected")) {
			if (currentSel->lic && !currentSel->multiple)
				debugPrint(3, "multiple options are selected");
			else {
				t->checked = t->rchecked = true;
				++currentSel->lic;
			}
		}
		if (!t->value)
			t->value = emptyString;
		t->textval = emptyString;
		if(optg && *optg) {
// borrow custom_h, opt group is like a custom header
			t->custom_h = cloneString(optg);
			optg = 0;
		}
		break;

	case TAGACT_STYLE:
		if (!opentag) {
			currentStyle = 0;
			break;
		}
		currentStyle = t;
		break;

	case TAGACT_SELECT:
	case TAGACT_DATAL:
		optg = 0;
		if (opentag) {
			currentSel = t;
			nopt = 0;
			t->itype = INP_SELECT;
			if(action == TAGACT_SELECT)
				formControl(t, true);
		} else {
			currentSel = 0;
			if(action == TAGACT_SELECT) {
				t->action = TAGACT_INPUT;
				t->value = displayOptions(t);
			}
		}
		break;

	case TAGACT_TA:
		if (opentag) {
			currentTA = t;
			t->itype = INP_TA;
			formControl(t, true);
		} else {
			t->action = TAGACT_INPUT;
			if (!t->value) {
/* This can only happen it no text inside, <textarea></textarea> */
/* like the other value fields, it can't be null */
				t->rvalue = t->value = emptyString;
			}
			currentTA = 0;
		}
		break;

	case TAGACT_META:
		if (opentag) {
/*********************************************************************
htmlMetaHelper is not called for document.createElement("meta")
I don't know if that matters.
Another problem which is extant in the result of a google search:
there is a meta tag with a refresh inside a noscript block.
The refresh shouldn't run at all, but it does,
and the next page has the same tags, and so on forever.
So don't do this if inside <noscript>, and js is active.
Are there other situations where we need to supress meta processing?
*********************************************************************/
			if(!(findOpenTag(t, TAGACT_NOSCRIPT) && isJSAlive))
				htmlMetaHelper(t);
		}
		break;

	case TAGACT_TBODY:
	case TAGACT_THEAD:
	case TAGACT_TFOOT:
		if (opentag)
			t->controller = findOpenTag(t, TAGACT_TABLE);
		break;

	case TAGACT_TR:
		if (opentag) {
			t->controller = findOpenSection(t);
			if (!t->controller)
				t->controller = findOpenTag(t, TAGACT_TABLE);
		}
		break;

	case TAGACT_TD:
		if (opentag)
			t->controller = findOpenTag(t, TAGACT_TR);
		break;

	case TAGACT_SPAN:
		if (!opentag)
			break;
		if (!(a = t->jclass))
			break;
		if (stringEqualCI(a, "sup"))
			action = TAGACT_SUP;
		if (stringEqualCI(a, "sub"))
			action = TAGACT_SUB;
		if (stringEqualCI(a, "ovb"))
			action = TAGACT_OVB;
		t->action = action;
		break;

	case TAGACT_OL:
/* look for start parameter for numbered list */
		if (opentag) {
			a = attribVal(t, "start");
			if (a && (j = stringIsNum(a)) >= 0)
				t->slic = j - 1;
		}
		break;

	case TAGACT_FRAME:
		if (opentag)
			break;
// If somebody wrote <frame><p>foo</frame>, those tags should be excised.
		underKill(t);
		break;

	case TAGACT_MUSIC:
		if (opentag)
			break;
// If somebody wrote <audio><p>foo</audio>, those tags should be excised.
// However <source> tags should be kept and/or expanded. Not yet implemented.
		underKill(t);
		break;

	}			/* switch */
}

void prerender(int start)
{
/* some cleanup routines to rearrange the tree */
	nestedAnchors(start);
	emptyAnchors(start);
	insert_tbody(start);
	tableForm(start);
linkPipe(start);

	currentForm = currentSel = currentOpt = NULL;
	currentTitle = currentScript = currentTA = NULL;
	currentStyle = NULL;
	optg = NULL;
	nzFree(radioCheck);
	radioCheck = 0;
	traverse_callback = prerenderNode;
	traverseAll(start);
	currentForm = NULL;
	nzFree(radioCheck);
	radioCheck = 0;
}

static char fakePropLast[24];
const char *fakePropName(void)
{
	static int idx = 0;
	sprintf(fakePropLast, "gc$%d", ++idx);
	return fakePropLast;
}

static void establish_inner(Tag *t, const char *start, const char *end,
			    bool isText)
{
	const char *s = emptyString;
	const char *name = (isText ? "value" : "innerHTML");
	if (start) {
		s = start;
		if (end)
			s = pullString(start, end - start);
	}
	set_property_string_t(t, name, s);
	if (start && end)
		nzFree((char *)s);
// If this is a textarea, we haven't yet set up the innerHTML
// getter and seter
	if (isText)
		set_property_string_t(t, "innerHTML", emptyString);
}

static const char defvl[] = "defaultValue";
static const char defck[] = "defaultChecked";
static const char defsel[] = "defaultSelected";

static void formControlJS(Tag *t)
{
	const char *typedesc;
	int itype = t->itype;
	int isradio = (itype == INP_RADIO);
	bool isselect = (itype == INP_SELECT);
	bool ista = (itype == INP_TA);
	const char *whichclass = (isselect ? "HTMLSelectElement" : (ista ? "HTMLTextAreaElement" : "HTMLInputElement"));
	const Tag *form = t->controller;

	if (form && form->jslink)
		domLink(t, whichclass, 0, "elements", form, isradio);
	else
		domLink(t, whichclass, 0, 0, 0, (4|isradio));
	if (!t->jslink)
		return;

	if (itype <= INP_RADIO && !isselect) {
		set_property_string_t(t, "value", t->value);
		if (itype != INP_FILE) {
/* No default value on file, for security reasons */
			set_property_string_t(t, defvl, t->value);
		}		/* not file */
	}

	if (isselect)
		typedesc = t->multiple ? "select-multiple" : "select-one";
	else
		typedesc = inp_types[itype];
	set_property_string_t(t, "type", typedesc);

	if (itype >= INP_RADIO) {
		set_property_bool_t(t, "checked", t->checked);
		set_property_bool_t(t, defck, t->checked);
	}
}

static void optionJS(Tag *t)
{
	Tag *sel = t->controller;
	const char *tx = t->textval;
	const char *cl = t->jclass;

	if (!sel)
		return;

	if (!tx) {
		debugPrint(3, "empty option");
	} else {
		if (!t->value)
			t->value = cloneString(tx);
	}
/* no point if the controlling select doesn't have a js object */
	if (!sel->jslink)
		return;
establish_js_option(t, sel);
// nodeName and nodeType set in constructor
	set_property_string_t(t, "text", t->textval);
	set_property_string_t(t, "value", t->value);
	set_property_bool_t(t, "selected", t->checked);
	set_property_bool_t(t, defsel, t->checked);
	if (!cl)
		cl = emptyString;
	set_property_string_t(t, "class", cl);
	set_property_string_t(t, "last$class", cl);

	if (t->checked && !sel->multiple)
		set_property_number_t(sel, "selectedIndex", t->lic);
}

static void link_css(Tag *t)
{
	struct i_get g;
	char *b;
	int blen;
	const char *a;
	const char *a1 = attribVal(t, "type");
	const char *a2 = attribVal(t, "rel");
	const char *altsource, *realsource;

	if (a1)
		set_property_string_t(t, "type", a1);
	if (a2)
		set_property_string_t(t, "rel", a2);
	if (!t->href)
		return;
	if (!stringEqualCI(a1, "text/css") &&
	    !stringEqualCI(a2, "stylesheet"))
		return;

// Fetch the css file so we can apply its attributes.
	a = NULL;
	altsource = fetchReplace(t->href);
	realsource = (altsource ? altsource : t->href);
	if ((browseLocal || altsource) && !isURL(realsource)) {
		debugPrint(3, "css source %s", realsource);
		if (!fileIntoMemory(realsource, &b, &blen)) {
			if (debugLevel >= 1)
				i_printf(MSG_GetLocalCSS);
		} else {
			a = force_utf8(b, blen);
			if (!a)
				a = b;
			else
				nzFree(b);
		}
	} else {
		debugPrint(3, "css source %s", t->href);
		memset(&g, 0, sizeof(g));
		g.thisfile = cf->fileName;
		g.uriEncoded = true;
		g.url = t->href;
		if (httpConnect(&g)) {
			nzFree(g.referrer);
			nzFree(g.cfn);
			if (g.code == 200) {
				a = force_utf8(g.buffer, g.length);
				if (!a)
					a = g.buffer;
				else
					nzFree(g.buffer);
// acid3 test[0] says we don't process this file if it's content type is
// text/html. Should I test for anything outside of text/css?
// For now I insist it be missing or text/css or text/plain.
// A similar test is performed in css.c after httpConnect.
				if (g.content[0]
				    && !stringEqual(g.content, "text/css")
				    && !stringEqual(g.content, "text/plain")) {
					debugPrint(3,
						   "css suppressed because content type is %s",
						   g.content);
					cnzFree(a);
					a = NULL;
				}
			} else {
				nzFree(g.buffer);
				if (debugLevel >= 3)
					i_printf(MSG_GetCSS, g.url, g.code);
			}
		} else {
			if (debugLevel >= 3)
				i_printf(MSG_GetCSS2);
		}
	}
	if (a) {
		set_property_string_t(t, "css$data", a);
// indicate we can run the onload function, if there is one
		t->lic = 1;
	}
	cnzFree(a);
}

static Tag *innerParent;

static void jsNode(Tag *t, bool opentag)
{
	const struct tagInfo *ti = t->info;
	int action = t->action;
	const Tag *above;
	const char *a;
	bool linked_in;

// run reindex at table close
	if (action == TAGACT_TABLE && !opentag && t->jslink)
		run_function_onearg_win(cf, "rowReindex", t);

/* all the js variables are on the open tag */
	if (!opentag)
		return;
	if (t->step >= 2)
		return;
	t->step = 2;

/*********************************************************************
If js is off, and you don't decorate this tree,
then js is turned on later, and you parse and decorate a frame,
it might also decorate this tree in the wrong context.
Needless to say that's not good!
*********************************************************************/
	if (t->f0 != cf)
		return;

	debugPrint(6, "decorate %s %d", t->info->name, t->seqno);
	fakePropLast[0] = 0;

	switch (action) {

	case TAGACT_TEXT:
		debugPrint(5, "domText");
		establish_js_textnode(t, fakePropName());
// nodeName and nodeType set in constructor
		if (t->jslink) {
			const char *w = t->textval;
			if (!w)
				w = emptyString;
			set_property_string_t(t, "data", w);
			w = (t->jclass ? t->jclass : emptyString);
			set_property_string_t(t, "class", w);
			set_property_string_t(t, "last$class", w);
		}
		break;

	case TAGACT_DOCTYPE:
		domLink(t, "DocType", 0, 0, 0, 4);
		t->deleted = true;
		break;

	case TAGACT_HTML:
		domLink(t, "HTML", 0, 0, 0, 4);
		cf->htmltag = t;
		break;

	case TAGACT_META:
		domLink(t, "HTMLMetaElement", 0, "metas", 0, 4);
		break;

	case TAGACT_STYLE:
		domLink(t, "HTMLStyleElement", 0, "styles", 0, 4);
		a = attribVal(t, "type");
		if (!a)
			a = emptyString;
		set_property_string_t(t, "type", a);
		break;

	case TAGACT_SCRIPT:
		domLink(t, "HTMLScriptElement", "src", "scripts", 0, 4);
		a = attribVal(t, "type");
		if (a)
			set_property_string_t(t, "type", a);
		a = attribVal(t, "text");
		if (a) {
			set_property_string_t(t, "text", a);
		} else {
			set_property_string_t(t, "text", "");
		}
		a = attribVal(t, "src");
		if (a) {
			set_property_string_t(t, "src", a);
			if (down_jsbg && a[0])	// from another source, let's get it started
				prepareScript(t);
		} else {
			set_property_string_t(t, "src", "");
		}
		break;

	case TAGACT_FORM:
		domLink(t, "HTMLFormElement", "action", "forms", 0, 4);
		break;

	case TAGACT_INPUT:
		formControlJS(t);
		if (t->itype == INP_TA)
			establish_inner(t, t->value, 0, true);
		break;

	case TAGACT_OPTION:
		optionJS(t);
// The parent child relationship has already been established,
// don't break, just return;
		return;

	case TAGACT_DATAL:
		domLink(t, "Datalist", 0, 0, 0, 4);
		break;

	case TAGACT_A:
		domLink(t, "HTMLAnchorElement", "href", "links", 0, 4);
		break;

	case TAGACT_HEAD:
		domLink(t, "HTMLHeadElement", 0, "heads", 0, 4);
		cf->headtag = t;
		break;

	case TAGACT_BODY:
		domLink(t, "HTMLBodyElement", 0, "bodies", 0, 4);
		cf->bodytag = t;
		break;

	case TAGACT_OL:
		domLink(t, "HTMLOListElement", 0, 0, 0, 4);
		break;
	case TAGACT_UL:
		domLink(t, "HTMLUListElement", 0, 0, 0, 4);
		break;
	case TAGACT_DL:
		domLink(t, "HTMLDListElement", 0, 0, 0, 4);
		break;

	case TAGACT_LI:
		domLink(t, "HTMLLIElement", 0, 0, 0, 4);
		break;

	case TAGACT_CANVAS:
		domLink(t, "HTMLCanvasElement", 0, 0, 0, 4);
		break;

	case TAGACT_TABLE:
		domLink(t, "HTMLTableElement", 0, "tables", 0, 4);
		break;

	case TAGACT_TBODY:
		if ((above = t->controller) && above->jslink)
			domLink(t, "tBody", 0, "tBodies", above, 0);
		break;

	case TAGACT_THEAD:
		if ((above = t->controller) && above->jslink) {
			domLink(t, "tHead", 0, 0, above, 0);
			set_property_object_t(above, "tHead", t);
		}
		break;

	case TAGACT_TFOOT:
		if ((above = t->controller) && above->jslink) {
			domLink(t, "tFoot", 0, 0, above, 0);
			set_property_object_t(above, "tFoot", t);
		}
		break;

	case TAGACT_TR:
		if ((above = t->controller) && above->jslink)
			domLink(t, "HTMLTableRowElement", 0, "rows", above, 0);
		break;

	case TAGACT_TD:
		if ((above = t->controller) && above->jslink)
			domLink(t, "HTMLTableCellElement", 0, "cells", above, 0);
		break;

	case TAGACT_DIV:
		domLink(t, "HTMLDivElement", 0, "divs", 0, 4);
		break;

	case TAGACT_LABEL:
		domLink(t, "HTMLLabelElement", 0, "labels", 0, 4);
		break;

	case TAGACT_OBJECT:
		domLink(t, "HtmlObj", 0, "htmlobjs", 0, 4);
		break;

	case TAGACT_UNKNOWN:
		domLink(t, "HTMLElement", 0, 0, 0, 4);
		break;

	case TAGACT_SPAN:
	case TAGACT_SUB:
	case TAGACT_SUP:
	case TAGACT_OVB:
		domLink(t, "HTMLSpanElement", 0, "spans", 0, 4);
		break;

	case TAGACT_AREA:
		domLink(t, "HTMLAreaElement", "href", "links", 0, 4);
		break;

	case TAGACT_FRAME:
// about:blank means a blank frame with no sourcefile.
		if (stringEqual(t->href, "about:blank")) {
			nzFree(t->href);
			t->href = 0;
		}
		domLink(t, "HTMLFrameElement", "src", 0, 0, 2);
		break;

	case TAGACT_IMAGE:
		domLink(t, "HTMLImageElement", "src", "images", 0, 4);
		break;

	case TAGACT_P:
		domLink(t, "HTMLParagraphElement", 0, "paragraphs", 0, 4);
		break;

	case TAGACT_H:
		domLink(t, "HTMLHeadingElement", 0, 0, 0, 4);
		break;

	case TAGACT_HEADER:
		domLink(t, "Header", 0, "headers", 0, 4);
		break;

	case TAGACT_FOOTER:
		domLink(t, "Footer", 0, "footers", 0, 4);
		break;

	case TAGACT_TITLE:
		if (cw->htmltitle)
			set_property_string_doc(cf, "title", cw->htmltitle);
		domLink(t, "Title", 0, 0, 0, 4);
		break;

	case TAGACT_LINK:
		domLink(t, "HTMLLinkElement", "href", 0, 0, 4);
		link_css(t);
		break;

	case TAGACT_MUSIC:
		domLink(t, "HTMLAudioElement", "src", 0, 0, 4);
		break;

	case TAGACT_BASE:
		domLink(t, "HTMLBaseElement", "href", 0, 0, 4);
		break;

	default:
// Don't know what this tag is, or it's not semantically important,
// so just call it an html element.
		domLink(t, "HTMLElement", 0, 0, 0, 4);
		break;
	}			/* switch */

	if (!t->jslink)
		return;		/* nothing else to do */

/* js tree mirrors the dom tree. */
	linked_in = false;

	if (t->parent && t->parent->jslink) {
		run_function_onearg_t(t->parent, "eb$apch1", t);
		linked_in = true;
	}

	if (action == TAGACT_HTML || action == TAGACT_DOCTYPE) {
		run_function_onearg_doc(cf, "eb$apch1", t);
		linked_in = true;
	}

	if (!t->parent && innerParent) {
// this is the top of innerHTML or some such.
// It is never html head or body, as those are skipped.
		run_function_onearg_t(innerParent, "eb$apch1", t);
		linked_in = true;
	}

	if (linked_in && fakePropLast[0]) {
// Node linked to document/gc to protect if from garbage collection,
// but now it is linked to its parent.
		delete_property_win(cf, fakePropLast);
	}

	if (!linked_in) {
		debugPrint(3, "tag %s not linked in", ti->name);
		if (action == TAGACT_TEXT)
			debugPrint(1, "text %s\n", t->textval);
	}

/* set innerHTML from the source html, if this tag supports it */
	if (ti->bits & TAG_INNERHTML)
		establish_inner(t, t->innerHTML, 0, false);

// If the tag has foo=bar as an attribute, pass this forward to javascript.
	pushAttributes(t);
}

static void pushAttributes(const Tag *t)
{
	int i;
	const char **a = t->attributes;
	const char **v = t->atvals;
	const char *u;
	char *x;
	if (!a)
		return;

	for (i = 0; a[i]; ++i) {
// There are some exceptions, some attributes that we handle individually.
// these are handled in domLink()
		static const char *const excdom[] = {
			"name", "id", "class", "classname",
			"checked", "value", "type",
			"href", "src", "action",
			0
		};
// These are on node.prototype, and you don't want to displace them.
// Sadly, you have to keep this list in sync with startwindow.js.
// These must be lower case, something about the attribute processing.
		static const char *const excprot[] = {
			"getelementsbytagname", "getelementsbyname",
			"getelementsbyclassname", "contains",
			"queryselectorall", "queryselector", "matches",
			"haschildnodes", "appendchild", "prependchild",
			"insertbefore", "replacechild",
			"eb$apch1", "eb$apch2", "eb$insbf", "removechild",
			"firstchild", "firstelementchild",
			"lastchild", "lastelementchild",
			"nextsibling", "nextelementsibling",
			"previoussibling", "previouselementsibling",
			"children",
			"hasattribute", "hasattributens", "markattribute",
			"getattribute", "getattributens",
			"setattribute", "setattributens",
			"removeattribute", "removeattributens",
			"parentelement",
			"getattributenode", "getclientrects",
			"clonenode", "importnode",
			"comparedocumentposition", "getboundingclientrect",
			"focus", "blur",
			"eb$listen", "eb$unlisten",
			"addeventlistener", "removeeventlistener",
			"attachevent", "detachevent", "dispatchevent",
			"insertadjacenthtml", "outerhtml",
			"injectsetup", "document_fragment_node",
			"classlist", "textcontent", "contenttext", "nodevalue",
// handlers are reserved yes, but they have important setters,
// so we do want to allow <A onclick=blah>
			0
		};
		static const char *const dotrue[] = {
			"required", "hidden", "aria-hidden",
			"multiple", "readonly", "disabled", "async", 0
		};

// html tags and attributes are case insensitive.
// That's why the entire getAttribute system drops names to lower case.
		u = v[i];
		if (!u)
			u = emptyString;
		x = cloneString(a[i]);
		caseShift(x, 'l');

// attributes on HTML tags that begin with "data-" should be available under a
// "dataset" object in JS
		if (strncmp(x, "data-", 5) == 0) {
// must convert to camelCase
			char *a2 = cloneString(x + 5);
			camelCase(a2);
			set_dataset_string_t(t, a2, u);
			nzFree(a2);
			run_function_onestring_t(t, "markAttribute",        x);
			nzFree(x);
			continue;
		}

		if (stringEqual(x, "style")) {	// no clue
			nzFree(x);
			continue;
		}

// check for exceptions.
// Maybe they wrote <a firstChild=foo>
		if( stringInList(excprot, a[i]) >= 0) {
			debugPrint(3, "html attribute overload %s.%s",
				   t->info->name, x);
			nzFree(x);
			continue;
		}

// There are some, like multiple or readonly, that should be set to true,
// not the empty string.
		if (stringInList(dotrue, a[i]) >= 0) {
			set_property_bool_t(t, x, !stringEqual(u, "false"));
		} else {
// standard attribute here
			                        if (stringInListCI(excdom, x) < 0)
				set_property_string_t(t, x, u);
		}
// special case, classname sets the class.
// Are there others like this?
		if(stringEqual(x, "classname"))
			run_function_onestring_t(t, "markAttribute", "class");
		else
			run_function_onestring_t(t, "markAttribute", x);
		nzFree(x);
	}
}

/* decorate the tree of nodes with js objects */
void decorate(int start)
{
	traverse_callback = jsNode;
	traverseAll(start);
}

/* paranoia check on the number of tags */
static void tagCountCheck(void)
{
	if (sizeof(int) == 4) {
		if (cw->numTags > MAXLINES)
			i_printfExit(MSG_LineLimit);
	}
}

static void pushTag(Tag *t)
{
	int a = cw->allocTags;
	if (cw->numTags == a) {
		debugPrint(4, "%d tags, %d dead", a, cw->deadTags);
/* make more room */
		a = a / 2 * 3;
		cw->tags =
		    (Tag **)reallocMem(cw->tags, a * sizeof(t));
		cw->allocTags = a;
	}
	tagList[cw->numTags++] = t;
	tagCountCheck();
}

static void freeTag(Tag *t);

// garbage collect the dead tags.
// You must rerender after this runs, so that the buffer has no dead tags,
// and the remaining tags have their new numbers embedded in the buffer.
void tag_gc(void)
{
	int cx;			/* edbrowse context */
	Window *w, *save_cw;
	Tag *t;
	int i, j;

	for (cx = 1; cx <= maxSession; ++cx) {
		for (w = sessionList[cx].lw; w; w = w->prev) {
			if (!w->tags)
				continue;
// Don't bother unless a third of the tags are dead.
			if (w->deadTags * 3 < w->numTags)
				continue;

// sync any changed fields before we muck with the tags.
			save_cw = cw;
			cw = w;
			cf = &(cw->f0);
			jSyncup(true, 0);
			cw = save_cw;
			cf = &(cw->f0);

// ok let's crunch.
			for (i = j = 0; i < w->numTags; ++i) {
				t = w->tags[i];
				if (t->dead) {
					freeTag(t);
				} else {
					t->seqno = j;
					w->tags[j++] = t;
				}
			}
			debugPrint(4, "tag_gc from %d to %d", w->numTags, j);
			w->numTags = j;
			w->deadTags = 0;

// We must rerender when we return to this window,
// or at the input loop if this is the current window.
// Tags have been renumbered, need to rebuild the text buffer accordingly.
			w->mustrender = true;
			if (w != cw)
				w->nextrender = 0;
		}
	}
}

// first one has to be the unknown
const struct tagInfo availableTags[] = {
	{"unknown0", "an html entity", TAGACT_UNKNOWN, 5, 1},
	{"doctype", "doctype", TAGACT_DOCTYPE, 0, 0},
	{"html", "html", TAGACT_HTML, 0, 0},
	{"base", "base reference for relative URLs", TAGACT_BASE, 0, 4},
	{"object", "an html object", TAGACT_OBJECT, 5, 3},
	{"a", "an anchor", TAGACT_A, 0, 1},
	{"htmlanchorelement", "an anchor element", TAGACT_A, 0, 1},
	{"input", "an input item", TAGACT_INPUT, 0, 4},
	{"element", "an input element", TAGACT_INPUT, 0, 4},
	{"title", "the title", TAGACT_TITLE, 0, 0},
	{"textarea", "an input text area", TAGACT_TA, 0, 0},
	{"select", "an option list", TAGACT_SELECT, 0, 0},
	{"datalist", "an input list", TAGACT_DATAL, 0, 0},
	{"option", "a select option", TAGACT_OPTION, 0, 0},
	{"optgroup", "an optiongroup", TAGACT_OPTG, 0, 0},
	{"sub", "a subscript", TAGACT_SUB, 0, 0},
	{"sup", "a superscript", TAGACT_SUP, 0, 0},
	{"ovb", "an overbar", TAGACT_OVB, 0, 0},
	{"font", "a font", TAGACT_NOP, 0, 0},
	{"cite", "a citation", TAGACT_NOP, 0, 0},
	{"tt", "teletype", TAGACT_NOP, 0, 0},
	{"center", "centered text", TAGACT_P, 2, 5},
	{"caption", "a caption", TAGACT_NOP, 5, 0},
	{"head", "the html header information", TAGACT_HEAD, 0, 5},
	{"body", "the html body", TAGACT_BODY, 0, 5},
	{"text", "a text section", TAGACT_TEXT, 0, 4},
	{"bgsound", "background music", TAGACT_MUSIC, 0, 4},
	{"audio", "audio passage", TAGACT_MUSIC, 0, 4},
	{"video", "video passage", TAGACT_MUSIC, 0, 4},
	{"meta", "a meta tag", TAGACT_META, 0, 4},
	{"style", "a style tag", TAGACT_STYLE, 0, 2},
	{"link", "a link tag", TAGACT_LINK, 0, 4},
	{"img", "an image", TAGACT_IMAGE, 0, 4},
	{"image", "an image", TAGACT_IMAGE, 0, 4},
	{"br", "a line break", TAGACT_BR, 1, 4},
	{"p", "a paragraph", TAGACT_P, 2, 5},
	{"fieldset", "a paragraph", TAGACT_NOP, 10, 1},
	{"blockquote", "a quoted section", TAGACT_BQ, 0, 1},
	{"header", "a header", TAGACT_HEADER, 2, 5},
	{"footer", "a footer", TAGACT_FOOTER, 2, 5},
	{"div", "a divided section", TAGACT_DIV, 5, 1},
	{"nav", "a navigation section", TAGACT_DIV, 5, 1},
	{"map", "a map of images", TAGACT_NOP, 5, 0},
	{"figure", "a figure", TAGACT_NOP, 10, 0},
	{"figcaption", "a figure caption", TAGACT_NOP, 10, 0},
	{"document", "a document", TAGACT_DOC, 5, 1},
	{"fragment", "a document fragment", TAGACT_FRAG, 5, 1},
	{"comment", "a comment", TAGACT_COMMENT, 0, 2},
	{"h1", "a level 1 header", TAGACT_H, 10, 1},
	{"h2", "a level 2 header", TAGACT_H, 10, 1},
	{"h3", "a level 3 header", TAGACT_H, 10, 1},
	{"h4", "a level 4 header", TAGACT_H, 10, 1},
	{"h5", "a level 5 header", TAGACT_H, 10, 1},
	{"h6", "a level 6 header", TAGACT_H, 10, 1},
	{"dt", "a term", TAGACT_DT, 2, 4},
	{"dd", "a definition", TAGACT_DD, 1, 4},
	{"li", "a list item", TAGACT_LI, 1, 5},
	{"ul", "a bullet list", TAGACT_UL, 10, 1},
	{"dir", "a directory list", TAGACT_NOP, 5, 0},
	{"menu", "a menu", TAGACT_NOP, 5, 0},
	{"ol", "a numbered list", TAGACT_OL, 10, 1},
	{"dl", "a definition list", TAGACT_DL, 10, 1},
	{"hr", "a horizontal line", TAGACT_HR, 5, 4},
	{"form", "a form", TAGACT_FORM, 10, 1},
	{"button", "a button", TAGACT_INPUT, 0, 1},
	{"frame", "a frame", TAGACT_FRAME, 2, 0},
	{"iframe", "a frame", TAGACT_FRAME, 2, 1},
	{"map", "an image map", TAGACT_MAP, 2, 4},
	{"area", "an image map area", TAGACT_AREA, 0, 4},
	{"table", "a table", TAGACT_TABLE, 10, 1},
	{"tbody", "a table body", TAGACT_TBODY, 0, 1},
	{"thead", "a table body", TAGACT_THEAD, 0, 1},
	{"tfoot", "a table body", TAGACT_TFOOT, 0, 1},
	{"tr", "a table row", TAGACT_TR, 5, 1},
	{"td", "a table entry", TAGACT_TD, 0, 5},
	{"th", "a table heading", TAGACT_TD, 0, 5},
	{"pre", "a preformatted section", TAGACT_PRE, 10, 0},
	{"listing", "a listing", TAGACT_PRE, 1, 0},
	{"xmp", "an example", TAGACT_PRE, 1, 0},
	{"fixed", "a fixed presentation", TAGACT_NOP, 1, 0},
	{"code", "a block of code", TAGACT_NOP, 0, 0},
	{"samp", "a block of sample text", TAGACT_NOP, 0, 0},
	{"address", "an address block", TAGACT_NOP, 1, 0},
	{"script", "a script", TAGACT_SCRIPT, 0, 1},
	{"noscript", "no script section", TAGACT_NOSCRIPT, 0, 2},
	{"noframes", "no frames section", TAGACT_NOP, 0, 2},
	{"embed", "embedded html", TAGACT_MUSIC, 0, 4},
	{"noembed", "no embed section", TAGACT_NOP, 0, 2},
	{"em", "emphasized text", TAGACT_JS, 0, 0},
	{"label", "a label", TAGACT_LABEL, 0, 0},
	{"strike", "emphasized text", TAGACT_JS, 0, 0},
	{"s", "emphasized text", TAGACT_JS, 0, 0},
	{"strong", "emphasized text", TAGACT_JS, 0, 0},
	{"b", "bold text", TAGACT_JS, 0, 0},
	{"i", "italicized text", TAGACT_JS, 0, 0},
	{"u", "underlined text", TAGACT_JS, 0, 0},
	{"var", "variable text", TAGACT_JS, 0, 0},
	{"kbd", "keyboard text", TAGACT_JS, 0, 0},
	{"dfn", "definition text", TAGACT_JS, 0, 0},
	{"q", "quoted text", TAGACT_JS, 0, 0},
	{"abbr", "an abbreviation", TAGACT_JS, 0, 0},
	{"span", "an html span", TAGACT_SPAN, 0, 1},
	{"svg", "an svg image", TAGACT_SVG, 0, 1},
	{"canvas", "a canvas", TAGACT_CANVAS, 0, 1},
	{"frameset", "a frame set", TAGACT_JS, 0, 0},
	{"", NULL, 0, 0, 0}
};

static void freeTag(Tag *t)
{
	char **a;
// Even if js has been turned off, if this tag was previously connected to an
// object, we should disconnect it.
	if(t->jslink)
		disconnectTagObject(t);
	nzFree(t->textval);
	nzFree(t->name);
	nzFree(t->id);
	nzFree(t->jclass);
	nzFree(t->nodeName);
	nzFree(t->value);
	cnzFree(t->rvalue);
	nzFree(t->href);
	nzFree(t->js_file);
	nzFree(t->innerHTML);
	nzFree(t->custom_h);

	a = (char **)t->attributes;
	if (a) {
		while (*a) {
			nzFree(*a);
			++a;
		}
		free(t->attributes);
	}

	a = (char **)t->atvals;
	if (a) {
		while (*a) {
			nzFree(*a);
			++a;
		}
		free(t->atvals);
	}

	free(t);
}

void freeTags(Window *w)
{
	int i, n;
	Tag *t, **e;

/* if not browsing ... */
	if (!(e = w->tags))
		return;

/* drop empty textarea buffers created by this session */
	for (t = w->inputlist; t; t = t->same) {
		if (t->action != TAGACT_INPUT)
			continue;
		if (t->itype != INP_TA)
			continue;
		if (!(n = t->lic))
			continue;
		freeEmptySideBuffer(n);
	}			/* loop over tags */

	for (i = 0; i < w->numTags; ++i, ++e) {
		t = *e;
		freeTag(t);
	}

	free(w->tags);
	w->tags = 0;
	w->numTags = w->allocTags = w->deadTags = 0;
	w->inputlist = w->scriptlist = w->optlist = w->linklist = 0;
	w->framelist = 0;
}

Tag *newTag(const Frame *f, const char *name)
{
	Tag *t, *t1, *t2 = 0;
	const struct tagInfo *ti;
	static int gsn = 0;

	for (ti = availableTags; ti->name[0]; ++ti)
		if (stringEqualCI(ti->name, name))
			break;

	if (!ti->name[0]) {
		debugPrint(4, "warning, created node %s reverts to generic",
			   name);
		ti = availableTags;
	}

	t = (Tag *)allocZeroMem(sizeof(Tag));
	t->action = ti->action;
	t->f0 = (Frame *) f;		/* set owning frame */
	t->info = ti;
	t->seqno = cw->numTags;
	t->gsn = ++gsn;
	t->nodeName = cloneString(name);
	pushTag(t);
	if (t->action == TAGACT_SCRIPT) {
		for (t1 = cw->scriptlist; t1; t1 = t1->same)
			if (!t1->slash)
				t2 = t1;
		if (t2)
			t2->same = t;
		else
			cw->scriptlist = t;
	}
	if (t->action == TAGACT_LINK) {
		for (t1 = cw->linklist; t1; t1 = t1->same)
			if (!t1->slash)
				t2 = t1;
		if (t2)
			t2->same = t;
		else
			cw->linklist = t;
	}
	if (t->action == TAGACT_FRAME) {
		for (t1 = cw->framelist; t1; t1 = t1->same)
			if (!t1->slash)
				t2 = t1;
		if (t2)
			t2->same = t;
		else
			cw->framelist = t;
	}
	if (t->action == TAGACT_INPUT || t->action == TAGACT_SELECT ||
	    t->action == TAGACT_TA) {
		for (t1 = cw->inputlist; t1; t1 = t1->same)
			if (!t1->slash)
				t2 = t1;
		if (t2)
			t2->same = t;
		else
			cw->inputlist = t;
	}
	if (t->action == TAGACT_OPTION) {
		for (t1 = cw->optlist; t1; t1 = t1->same)
			if (!t1->slash)
				t2 = t1;
		if (t2)
			t2->same = t;
		else
			cw->optlist = t;
	}
	return t;
}

/* reserve space for 512 tags */
void initTagArray(void)
{
	cw->numTags = 0;
	cw->allocTags = 512;
	cw->deadTags = 0;
	cw->tags =
	    (Tag **)allocMem(cw->allocTags *
					sizeof(Tag *));
}

bool htmlGenerated;
static Tag *treeAttach;
static int tree_pos;
static bool treeDisable;
static void intoTree(Tag *parent);
static const int tdb = 5;	// tree debug level

void htmlNodesIntoTree(int start, Tag *attach)
{
	treeAttach = attach;
	tree_pos = start;
	treeDisable = false;
	debugPrint(tdb, "@@tree of nodes");
	intoTree(0);
	debugPrint(tdb, "}\n@@end tree");
}

/* Convert a list of html nodes, properly nested open close, into a tree.
 * Attach the tree to an existing tree here, for document.write etc,
 * or just build the tree if attach is null. */
static void intoTree(Tag *parent)
{
	Tag *t, *prev = 0;
	int j;
	const char *v;
	int action;

	if (!parent)
		debugPrint(tdb, "root {");
	else
		debugPrint(tdb, "%s %d {", parent->info->name, parent->seqno);

	while (tree_pos < cw->numTags) {
		t = tagList[tree_pos++];
		if (t->slash) {
			if (parent) {
				parent->balance = t, t->balance = parent;
				t->dead = parent->dead;
				if (t->dead)
					++cw->deadTags;
			}
			debugPrint(tdb, "}");
			return;
		}

		if (treeDisable) {
			debugPrint(tdb, "node skip %s", t->info->name);
			t->dead = true;
			++cw->deadTags;
			intoTree(t);
			continue;
		}

		if (htmlGenerated) {
/*Some things are different if the html is generated, not part of the original web page.
 * You can skip past <head> altogether, including its
 * tidy generated descendants, and you want to pass through <body>
 * to the children below. */
			action = t->action;
			if (action == TAGACT_HEAD) {
				debugPrint(tdb, "node skip %s", t->info->name);
				t->dead = true;
				++cw->deadTags;
				treeDisable = true;
				intoTree(t);
				treeDisable = false;
				continue;
			}
			if (action == TAGACT_DOCTYPE || action == TAGACT_HTML || action == TAGACT_BODY) {
				debugPrint(tdb, "node pass %s", t->info->name);
				t->dead = true;
				++cw->deadTags;
				intoTree(t);
				continue;
			}

/* this node is ok, but if parent is a pass through node... */
			if (parent == 0 ||	/* this shouldn't happen */
			    parent->action == TAGACT_BODY) {
/* link up to treeAttach */
				const char *w = "root";
				if (treeAttach)
					w = treeAttach->info->name;
				debugPrint(tdb, "node up %s to %s",
					   t->info->name, w);
				t->parent = treeAttach;
				if (treeAttach) {
					Tag *c =
					    treeAttach->firstchild;
					if (!c)
						treeAttach->firstchild = t;
					else {
						while (c->sibling)
							c = c->sibling;
						c->sibling = t;
					}
				}
				goto checkattributes;
			}
		}

/* regular linking through the parent node */
/* Could be treeAttach if this is a frame inside a window */
		t->parent = (parent ? parent : treeAttach);
		if (prev) {
			prev->sibling = t;
		} else if (parent) {
			parent->firstchild = t;
		} else if (treeAttach) {
			treeAttach->firstchild = t;
		}
		prev = t;

checkattributes:
/* check for some common attributes here */
		action = t->action;
		if (stringInListCI(t->attributes, "onclick") >= 0)
			t->onclick = t->doorway = true;
		if (stringInListCI(t->attributes, "onchange") >= 0)
			t->onchange = t->doorway = true;
		if (stringInListCI(t->attributes, "onsubmit") >= 0)
			t->onsubmit = t->doorway = true;
		if (stringInListCI(t->attributes, "onreset") >= 0)
			t->onreset = t->doorway = true;
		if (stringInListCI(t->attributes, "onload") >= 0)
			t->onload = t->doorway = true;
		if (stringInListCI(t->attributes, "onunload") >= 0)
			t->onunload = t->doorway = true;
		if (stringInListCI(t->attributes, "checked") >= 0)
			t->checked = t->rchecked = true;
		if (stringInListCI(t->attributes, "readonly") >= 0)
			t->rdonly = true;
		if (stringInListCI(t->attributes, "disabled") >= 0)
			t->disabled = true;
		if (stringInListCI(t->attributes, "multiple") >= 0)
			t->multiple = true;
		if (stringInListCI(t->attributes, "required") >= 0)
			t->required = true;
		if (stringInListCI(t->attributes, "async") >= 0)
			t->async = true;
		if ((j = stringInListCI(t->attributes, "name")) >= 0) {
/* temporarily, make another copy; some day we'll just point to the value */
			v = t->atvals[j];
			if (v && !*v)
				v = 0;
			t->name = cloneString(v);
		}
		if ((j = stringInListCI(t->attributes, "id")) >= 0) {
			v = t->atvals[j];
			if (v && !*v)
				v = 0;
			t->id = cloneString(v);
		}
		if ((j = stringInListCI(t->attributes, "class")) >= 0) {
			v = t->atvals[j];
			if (v && !*v)
				v = 0;
			t->jclass = cloneString(v);
		}
// classname is an alias for class
		if ((j = stringInListCI(t->attributes, "classname")) >= 0) {
			v = t->atvals[j];
			if (v && !*v)
				v = 0;
			t->jclass = cloneString(v);
		}
		if ((j = stringInListCI(t->attributes, "value")) >= 0) {
			v = t->atvals[j];
			if (v && !*v)
				v = 0;
			t->value = cloneString(v);
			t->rvalue = cloneString(v);
		}
// Resolve href against the base, but wait a minute, what if it's <p href=blah>
// and we're not suppose to resolve it? I don't ask about the parent node.
// Well, in general, I don't carry the href attribute into the js node.
// I only do it when it is relevant, such as <a> or <area>.
// See the exceptions in pushAttributes() in this file.
// I know, it's confusing.
		if ((j = stringInListCI(t->attributes, "href")) >= 0) {
			v = t->atvals[j];
			if (v && !*v)
				v = 0;
			if (v) {
				v = resolveURL(cf->hbase, v);
				cnzFree(t->atvals[j]);
				t->atvals[j] = v;
				if (action == TAGACT_BASE && !cf->baseset) {
					nzFree(cf->hbase);
					cf->hbase = cloneString(v);
					cf->baseset = true;
				}
				t->href = cloneString(v);
			}
		}
		if ((j = stringInListCI(t->attributes, "src")) >= 0) {
			v = t->atvals[j];
			if (v && !*v)
				v = 0;
			if (v) {
				v = resolveURL(cf->hbase, v);
				cnzFree(t->atvals[j]);
				t->atvals[j] = v;
				if (!t->href)
					t->href = cloneString(v);
			}
		}
		if ((j = stringInListCI(t->attributes, "action")) >= 0) {
			v = t->atvals[j];
			if (v && !*v)
				v = 0;
			if (v) {
				v = resolveURL(cf->hbase, v);
				cnzFree(t->atvals[j]);
				t->atvals[j] = v;
				if (!t->href)
					t->href = cloneString(v);
			}
		}

/* href=javascript:foo() is another doorway into js */
		if (t->href && memEqualCI(t->href, "javascript:", 11))
			t->doorway = true;
/* And of course the primary doorway */
		if (action == TAGACT_SCRIPT) {
			t->doorway = true;
			t->scriptgen = htmlGenerated;
		}

		intoTree(t);
	}
}

/* Parse some html, as generated by innerHTML or document.write. */
void html_from_setter(Tag *t, const char *h)
{
	int l = cw->numTags;

	debugPrint(3, "parse html from innerHTML");
	debugPrint(4, "parse under tag %s %d", t->info->name, t->seqno);
	debugGenerated(h);

// Cut all the children away from t
	underKill(t);

	html2nodes(h, false);
	htmlGenerated = true;
	htmlNodesIntoTree(l, t);
	prerender(0);
	innerParent = t;
	decorate(0);
	innerParent = 0;
	debugPrint(3, "end parse html from innerHTML");
}

void debugGenerated(const char *h)
{
	const char *h1, *h2;
	h1 = strstr(h, "<body>"); // it should be there
	h1 = h1 ? h1 + 6 : h;
// and </body> should be at the end
	h2 = h + strlen(h) - 7;
// Yeah this is one of those times I override const, but I put it back,
// so it's like a const string.
	*(char*)h2 = 0;
	if(debugLevel == 3) {
		if(strlen(h1) >= 200)
			debugPrint(3, "Generated ↑long↑");
		else
			debugPrint(3, "Generated ↑%s↑", h1);
	} else
		debugPrint(4, "Generated ↑%s↑", h1);
	*(char*)h2 = '<';
}

// Find window and frame based on the js context. Set cw and cf accordingly.
// This is inefficient, but is not called very often.
bool frameFromContext(jsobjtype cx)
{
	int i;
	Window *w;
	Frame *f;
	for (i = 0; i < MAXSESSION; ++i) {
		for (w = sessionList[i].lw; w; w = w->prev) {
			for (f = &(w->f0); f; f = f->next) {
				if(f->cx == cx) {
					cf = f, cw = w;
					return true;
				}
			}
		}
	}
	debugPrint(3, "frameFromContext cannot find the frame, job is not executed");
	return false;
}
