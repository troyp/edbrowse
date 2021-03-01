#include <quickjs/quickjs-libc.h>

#include "eb.h"


#define TOP 3

// stubs needed by other edbrowse functions that we are pulling in.
int context; // edbrowse context, not js
struct ebWindow *cw;
struct ebSession sessionList[TOP];
bool cxCompare(int cx) { return false; }
bool cxActive(int cx) { return false; }
bool cxQuit(int cx, int action) { return true; }
void cxSwitch(int cx, bool interactive) {}
bool browseCurrentBuffer(void) { return false; }
int sideBuffer(int cx, const char *text, int textlen, const char *bufname){ return 0; }
struct MACCOUNT accounts[MAXACCOUNT];
int maxAccount;		/* how many email accounts specified */
void preFormatCheck(int tagno, bool * pretag, bool * slash) {}
bool isDataURI(const char *u){ return false; }
void unpercentString(char *s) {}

// sample native function
static JSValue nat_puts(JSContext *cx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
if(argc >= 1) {
    const char *str = JS_ToCString(cx, argv[0]);
            if (str) {
printf("%s", str);
JS_FreeCString(cx, str);
            }
        }
printf("\n");
    return JS_UNDEFINED;
}

static JSValue test_getter(JSContext *cx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
return JS_NewAtomString(cx, "come on baby light my fire");
}

static JSValue test_setter(JSContext *cx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
int32_t z = 0, y;
if(argc >= 1) {
// I just assume the arg is an int.
if(JS_ToInt32(cx, &y, argv[0])) // failure to convert
z = 666;
else
z = y;
}
printf("setter %d\n", z);
return JS_UNDEFINED;
}

int main(int argc, char **argv)
{
int c;
JSRuntime *rt;
JSContext *cx[3];
JSValue wo[TOP]; // window objects
const char *filename = "interactive";
JSValue val;
const char *result;
const char *first = "'hello world, the answer is ' + 6*7;";
JSAtom a;
char line[80];

selectLanguage();

// test run; let's see the errors
debugLevel = 4;

rt = JS_NewRuntime();
for(c=0; c<TOP; ++c) {
cx[c] = JS_NewContext(rt);
wo[c] = JS_GetGlobalObject(cx[c]);
}

// as though windows 2 and 3 are frames in window 1
JS_DefinePropertyValueStr(cx[0], wo[0], "f2", wo[1], JS_PROP_ENUMERABLE);
JS_DefinePropertyValueStr(cx[0], wo[0], "f3", wo[2], JS_PROP_ENUMERABLE);

/*********************************************************************
Sample native method, eb$puts, just like edbrowse.
You can use setPropertyStr and that works, but if you want
nonstandard attributes, like not writable, then you have to do this.
*********************************************************************/
    JS_DefinePropertyValueStr(cx[0], wo[0], "eb$puts",
JS_NewCFunction(cx[0], nat_puts, "eb$puts", 1), 0);

/*********************************************************************
sample getter setter, just in window 1.
Unfortunately there is no GetSetStr, so I can name the property as a string.
I have to create an atom for the property name, ugh, but then,
am I suppose to free it or does DefineProperty take responsibility for it?
The program runs properly whether I free it or not, so idk.
*********************************************************************/
a = JS_NewAtom(cx[0], "gs");
JS_DefinePropertyGetSet(cx[0], wo[0], a,
JS_NewCFunction(cx[0], test_getter, "testget", 0),
JS_NewCFunction(cx[0], test_setter, "testset", 0),
JS_PROP_ENUMERABLE);
JS_FreeAtom(cx[0], a);

/*********************************************************************
I believe that JS_DefinePropertyValue takes over responsibility for the value,
that is, it is now managed in the javascript world.
We should not free it and when I try, FreeRuntime blows up.
wo[1] and wo[2] are owned by f2 and f3 respectively.
However, the first window object wo[0] is never passed by value
to js, so that should be freed.
If I don't, FreeRuntime blows up.
*********************************************************************/
JS_FreeValue(cx[0], wo[0]);

// sample execution in the first window
c = 0;
val = JS_Eval(cx[c], first, strlen(first), filename, JS_EVAL_TYPE_GLOBAL);
result = JS_ToCString(cx[c], val);
puts(result);
// do we have to free these in reverse order?
JS_FreeCString(cx[c], result);
JS_FreeValue(cx[c], val);

while(fgets(line, sizeof(line), stdin)) {
int l = strlen(line);
// chomp
if(l && line[l-1] == '\n') line[--l] = 0;
if(!l) // empty line
continue;
if(stringEqual(line, "q") || stringEqual(line, "qt"))
break;

// show context
if(stringEqual(line, "e")) {
printf("session %d\n", c+1);
continue;
}

// change context
if(line[0] == 'e' &&
line[1] >= '1' && line[1] <= '0'+TOP &&
!line[2]) {
static char tempname[16];
sprintf(tempname, "session %c", line[1]);
puts(tempname);
c = line[1] - '1';
continue;
}

if(line[0] == '<') { // from a file
char *data;
int datalen;
if(!fileIntoMemory(line+1, &data, &datalen)) {
showError();
continue;
}
val = JS_Eval(cx[c], data, datalen, line + 1, JS_EVAL_TYPE_GLOBAL);
nzFree(data);
if(JS_IsException(val)) {
js_std_dump_error(cx[c]);
} else {
puts("ok");
}
JS_FreeValue(cx[c], val);
continue;
}

val = JS_Eval(cx[c], line, strlen(line), filename, JS_EVAL_TYPE_GLOBAL);
if(JS_IsException(val)) {
js_std_dump_error(cx[c]);
} else {
result = JS_ToCString(cx[c], val);
puts(result);
JS_FreeCString(cx[c], result);
}
JS_FreeValue(cx[c], val);
}

// clean up
for(c=0; c<TOP; ++c)
JS_FreeContext(cx[c]);

JS_FreeRuntime(rt);
}

void ebClose(int n)
{
	exit(n);
}