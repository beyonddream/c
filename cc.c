#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "util.h"
#include "cpp.h"
#include "ctypes.h"
#include "cc.h"
#include "ir.h"

static CTy   *declspecs(int *);
static CTy   *ptag(void);
static CTy   *pstruct(int);
static CTy   *penum(void);
static CTy   *typename(void);
static CTy   *declarator(CTy *, char **, Node **);
static CTy   *directdeclarator(CTy *, char **);
static CTy   *declaratortail(CTy *);
static void   funcbody(void);
static void   block(void);
static void   stmt(void);
static void   pif (void);
static void   pfor(void);
static void   dowhile(void);
static void   pwhile(void);
static void   block(void);
static void   preturn(void);
static void   pswitch(void);
static void   pdefault(void);
static void   pcase(void);
static void   pcontinue(void);
static void   pbreak(void);
static void   stmt(void);
static void   exprstmt(void);
static Const *constexpr(void);
static Node  *expr(void);
static Node  *assignexpr(void);
static Node  *condexpr(void);
static Node  *logorexpr(void);
static Node  *logandexpr(void);
static Node  *orexpr(void);
static Node  *xorexpr(void);
static Node  *andexpr(void);
static Node  *eqlexpr(void);
static Node  *relexpr(void);
static Node  *shiftexpr(void);
static Node  *addexpr(void);
static Node  *mulexpr(void);
static Node  *castexpr(void);
static Node  *unaryexpr(void);
static Node  *postexpr(void);
static Node  *primaryexpr(void);
static Node  *vastart(void);
static Node  *ipromote(Node *);
static Node  *declinit(CTy *);
static CTy   *usualarithconv(Node **, Node **);
static Node  *mkcast(SrcPos *, Node *, CTy *);
static Node  *mknode(int type, SrcPos *p);
static IRVal  compileexpr(Node *n);
static IRVal  compilebinop(Node *n);


Tok *tok;
Tok *nexttok;

static void
next(void)
{
	tok = nexttok;
	nexttok = pp();
}

static void
expect(Tokkind kind)
{
	if (tok->k != kind)
		errorposf(&tok->pos,"expected %s, got %s", 
			tokktostr(kind), tokktostr(tok->k));
	next();
}


#define MAXSCOPES 1024
static int nscopes;
static Map *tags[MAXSCOPES];
static Map *syms[MAXSCOPES];

typedef struct Switch {

} Switch;

#define MAXLABELDEPTH 2048
static int     switchdepth;
static int     brkdepth;
static int     contdepth;
static char   *breaks[MAXLABELDEPTH];
static char   *conts[MAXLABELDEPTH];
static Switch *switches[MAXLABELDEPTH];

Vec  *tentativesyms;

Sym  *curfunc;

typedef struct GotoLabel {
	int defined;
	char *backendlabel;
} GotoLabel;

typedef struct Goto {
	SrcPos pos;
	GotoLabel *label;
} Goto;


Map  *labels;
Vec  *gotos;

static GotoLabel *
lookuplabel(char *name)
{
	GotoLabel *label;

	label = mapget(labels, name);
	if (!label) {
		label = xmalloc(sizeof(GotoLabel));
		label->backendlabel = newlabel();
		label->defined = 0;
		mapset(labels, name, label);
	}
	return label;
}

static void
pushswitch (Switch *s)
{
        switches[switchdepth] = s;
        switchdepth += 1;
}

static void
popswitch (void)
{
        switchdepth -= 1;
        if (switchdepth < 0)
                panic("internal error");
}

static Switch *
curswitch (void)
{
        if (switchdepth == 0)
                return 0;
        return switches[switchdepth - 1];
}

static void
pushcontbrk(char *lcont, char *lbreak)
{
        conts[contdepth] = lcont;
        breaks[brkdepth] = lbreak;
        brkdepth += 1;
        contdepth += 1;
}

static void
popcontbrk(void)
{
        brkdepth -= 1;
        contdepth -= 1;
        if (brkdepth < 0 || contdepth < 0)
                panic("internal error");
}

static char *
curcont()
{
        if (contdepth == 0)
                return 0;
        return conts[contdepth - 1];
}

static char *
curbrk()
{
        if (brkdepth == 0)
                return 0;
        return breaks[brkdepth - 1];
}

static void
pushbrk(char *lbreak)
{
        breaks[brkdepth] = lbreak;
        brkdepth += 1;
}

static void
popbrk(void)
{
	brkdepth -= 1;
	if(brkdepth < 0)
		panic("internal error");
}

static void
popscope(void)
{
        nscopes -= 1;
        if (nscopes < 0)
                errorf("bug: scope underflow\n");
        syms[nscopes] = 0;
        tags[nscopes] = 0;
}

static void
pushscope(void)
{
        syms[nscopes] = map();
        tags[nscopes] = map();
        nscopes += 1;
        if (nscopes > MAXSCOPES)
                errorf("scope depth exceeded maximum\n");
}

static int
isglobal(void)
{
        return nscopes == 1;
}


static int
define(Map *scope[], char *k, void *v)
{
	Map *m;
	
	m = scope[nscopes - 1];
	if (mapget(m, k))
		return 0;
	mapset(m, k, v);
	return 1; 
}

static void *
lookup(Map *scope[], char *k)
{
	int   i;
	void *v;
	
	i = nscopes;
	while (i--) {
		v = mapget(scope[i], k);
		if (v)
			return v;
	}
	return 0;
}

/* TODO: proper efficient set for tentative syms */
static void
removetentativesym(Sym *sym)
{
	int i;
	Vec *newv;
	Sym *s;

	newv = vec();
	for (i = 0; i < tentativesyms->len; i++) {
		s = vecget(tentativesyms, i);
		if (s == sym)
			continue;
		vecappend(newv, s);
	}
	tentativesyms = newv;
}

static void
addtentativesym(Sym *sym)
{
	int i;
	Sym *s;

	for (i = 0; i < tentativesyms->len; i++) {
		s = vecget(tentativesyms, i);
		if (s == sym)
			return;
	}
	vecappend(tentativesyms, sym);
}

static Sym *
defineenum(SrcPos *p, char *name, CTy *type, int64 v)
{
	Sym *sym;

	sym = xmalloc(sizeof(Sym));
	sym->pos = p;
	sym->name = name;
	sym->type = type;
	sym->k = SYMENUM;
	sym->Enum.v = v;
	if (!define(syms, name, sym))
		errorposf(p, "redefinition of %s", name);
	return sym;
}

static Sym *
definesym(SrcPos *p, int sclass, char *name, CTy *type, Node *n)
{
	Sym *sym;

	if (sclass == SCAUTO || n != 0)
		if (type->incomplete)
			errorposf(p, "cannot use incomplete type in this context");
	if (sclass == SCAUTO && isglobal())
		errorposf(p, "defining local symbol in global scope");
	sym = mapget(syms[nscopes - 1], name);
	if (sym) {
		switch (sym->k) {
		case SYMTYPE:
			if (sclass != SCTYPEDEF || !sametype(sym->type, type))
				errorposf(p, "incompatible redefinition of typedef %s", name);
			break;
		case SYMGLOBAL:
			if (sym->Global.sclass == SCEXTERN && sclass == SCGLOBAL)
				sym->Global.sclass = SCGLOBAL;
			if (sym->Global.sclass != sclass)
				errorposf(p, "redefinition of %s with differing storage class", name);
			if (sym->init && n)
				errorposf(p, "%s already initialized", name);
			if (!sym->init && n) {
				sym->init = n;
				if (!isfunc(sym->type))
					emitsym(sym);
				removetentativesym(sym);
			}
			break;
		default:
			errorposf(p, "redefinition of %s", name);
		}
		return sym;
	}
	
	sym = xmalloc(sizeof(Sym));
	sym->pos = p;
	sym->name = name;
	sym->type = type;
	sym->init = n;
	
	switch (sclass) {
	case SCAUTO:
		sym->k = SYMLOCAL;
		sym->Local.addr = alloclocal(sym->type);
		break;
	case SCTYPEDEF:
		sym->k = SYMTYPE;
		break;
	case SCEXTERN:
		sym->k = SYMGLOBAL;
		sym->Global.label = name;
		sym->Global.sclass = SCEXTERN;
		break;
	case SCGLOBAL:
		sym->k = SYMGLOBAL;
		sym->Global.label = name;
		sym->Global.sclass = SCGLOBAL;
		break;
	case SCSTATIC:
		sym->k = SYMGLOBAL;
		sym->Global.label = newlabel();
		sym->Global.sclass = SCSTATIC;
		break;
	default:
		panic("internal error");
	}
	if (!isfunc(sym->type)) {
		if (sym->k == SYMGLOBAL) {
			if (sym->init || sym->Global.sclass == SCEXTERN)
				emitsym(sym);
			else
				addtentativesym(sym);
		}
	}
	if (!define(syms, name, sym))
		panic("internal error");
	
	return sym;
}


static void
params(CTy *fty)
{
	int     sclass;
	CTy    *t;
	char   *name;
	SrcPos *pos;

	fty->Func.isvararg = 0;
	if (tok->k == ')')
		return;
	if (tok->k == TOKVOID && nexttok->k == ')') {
		next();
		return;
	}
	for (;;) {
		pos = &tok->pos;
		t = declspecs(&sclass);
		t = declarator(t, &name, 0);
		if (sclass != SCNONE)
			errorposf(pos, "storage class not allowed in parameter decl");
		vecappend(fty->Func.params, newnamety(name, t));
		if (tok->k != ',')
			break;
		next();
		if (tok->k == TOKELLIPSIS) {
			fty->Func.isvararg = 1;
			next();
			break;
		}
	}
}

static void
decl()
{
	Node   *init;
	char   *name;
	CTy    *type, *basety;
	SrcPos *pos;
	Sym    *sym;
	Vec    *syms;
	int     sclass;

	pos = &tok->pos;
	syms  = vec();
	basety = declspecs(&sclass);
	while (tok->k != ';' && tok->k != TOKEOF) {
		type = declarator(basety, &name, &init);
		switch (sclass){
		case SCNONE:
			if (isglobal()) {
				sclass = SCGLOBAL;
			} else {
				sclass = SCAUTO;
			}
			break;
		case SCTYPEDEF:
			if (init)
				errorposf(pos, "typedef cannot have an initializer");
			break;
		}
		if (!name)
			errorposf(pos, "decl needs to specify a name");
		sym = definesym(pos, sclass, name, type, init);
		vecappend(syms, sym);
		if (isglobal() && tok->k == '{') {
			if (init)
				errorposf(pos, "function declaration has an initializer");
			if (type->t != CFUNC)
				errorposf(pos, "expected a function");
			curfunc = sym;
			emitfuncstart(sym);
			funcbody();
			emitfuncend();
			definesym(pos, sclass, name, type, (Node*)1);
			curfunc = 0;
			return;
		}
		if (tok->k == ',')
			next();
		else
			break;
	}
	expect(';');

}

static int
issclasstok(Tok *t) {
	switch (t->k) {
	case TOKEXTERN:
	case TOKSTATIC:
	case TOKREGISTER:
	case TOKTYPEDEF:
	case TOKAUTO:
		return 1;
	default:
		return 0;
	}
}

static CTy *
declspecs(int *sclass)
{
	CTy    *t;
	SrcPos *pos;
	Sym    *sym;
	int     bits;

	enum {
		BITCHAR = 1<<0,
		BITSHORT = 1<<1,
		BITINT = 1<<2,
		BITLONG = 1<<3,
		BITLONGLONG = 1<<4,
		BITSIGNED = 1<<5,
		BITUNSIGNED = 1<<6,
		BITFLOAT = 1<<7,
		BITDOUBLE = 1<<8,
		BITENUM = 1<<9,
		BITSTRUCT = 1<<10,
		BITVOID = 1<<11,
		BITIDENT = 1<<12,
	};

	t = 0;
	bits = 0;
	pos = &tok->pos;
	*sclass = SCNONE;

	for(;;) {
		if (issclasstok(tok)) {
			if (*sclass != SCNONE)
				errorposf(pos, "multiple storage classes in declaration specifiers.");
			switch (tok->k) {
			case TOKEXTERN:
				*sclass = SCEXTERN;
				break;
			case TOKSTATIC:
				*sclass = SCSTATIC;
				break;
			case TOKREGISTER:
				*sclass = SCREGISTER;
				break;
			case TOKAUTO:
				*sclass = SCAUTO;
				break;
			case TOKTYPEDEF:
				*sclass = SCTYPEDEF;
				break;
			default:
				panic("internal error");
			}
			next();
			continue;
		}
		switch (tok->k) {
		case TOKCONST:
		case TOKVOLATILE:
			next();
			break;
		case TOKSTRUCT:
		case TOKUNION:
			if (bits)
				goto err;
			bits |= BITSTRUCT;
			t = ptag();
			goto done;
		case TOKENUM:
			if (bits)
				goto err;
			bits |= BITENUM;
			t = ptag();
			goto done;
		case TOKVOID:
			if (bits&BITVOID)
				goto err;
			bits |= BITVOID;
			next();
			goto done;
		case TOKCHAR:
			if (bits&BITCHAR)
				goto err;
			bits |= BITCHAR;
			next();
			break;
		case TOKSHORT:
			if (bits&BITSHORT)
				goto err;
			bits |= BITSHORT;
			next();
			break;
		case TOKINT:
			if (bits&BITINT)
				goto err;
			bits |= BITINT;
			next();
			break;
		case TOKLONG:
			if (bits&BITLONGLONG)
				goto err;
			if (bits&BITLONG) {
				bits &= ~BITLONG;
				bits |= BITLONGLONG;
			} else {
				bits |= BITLONG;
			}
			next();
			break;
		case TOKFLOAT:
			if (bits&BITFLOAT)
				goto err;
			bits |= BITFLOAT;
			next();
			break;
		case TOKDOUBLE:
			if (bits&BITDOUBLE)
				goto err;
			bits |= BITDOUBLE;
			next();
			break;
		case TOKSIGNED:
			if (bits&BITSIGNED)
				goto err;
			bits |= BITSIGNED;
			next();
			break;
		case TOKUNSIGNED:
			if (bits&BITUNSIGNED)
				goto err;
			bits |= BITUNSIGNED;
			next();
			break;
		case TOKIDENT:
			sym = lookup(syms, tok->v);
			if (sym && sym->k == SYMTYPE)
				t = sym->type;
			if (t && !bits) {
				bits |= BITIDENT;
				next();
				goto done;
			}
			/* fallthrough */
		default:
			goto done;
		}
	}
	done:
	switch (bits){
	case BITFLOAT:
		return cfloat;
	case BITDOUBLE:
		return cdouble;
	case BITLONG|BITDOUBLE:
		return cldouble;
	case BITSIGNED|BITCHAR:
	case BITCHAR:
		return cchar;
	case BITUNSIGNED|BITCHAR:
		return cuchar;
	case BITSIGNED|BITSHORT|BITINT:
	case BITSHORT|BITINT:
	case BITSHORT:
		return cshort;
	case BITUNSIGNED|BITSHORT|BITINT:
	case BITUNSIGNED|BITSHORT:
		return cushort;
	case BITSIGNED|BITINT:
	case BITSIGNED:
	case BITINT:
	case 0:
		return cint;
	case BITUNSIGNED|BITINT:
	case BITUNSIGNED:
		return cuint;
	case BITSIGNED|BITLONG|BITINT:
	case BITSIGNED|BITLONG:
	case BITLONG|BITINT:
	case BITLONG:
		return clong;
	case BITUNSIGNED|BITLONG|BITINT:
	case BITUNSIGNED|BITLONG:
		return culong;
	case BITSIGNED|BITLONGLONG|BITINT:
	case BITSIGNED|BITLONGLONG:
	case BITLONGLONG|BITINT:
	case BITLONGLONG:
		return cllong;
	case BITUNSIGNED|BITLONGLONG|BITINT:
	case BITUNSIGNED|BITLONGLONG:
		return cullong;
	case BITVOID:
		t = cvoid;
		return t;
	case BITENUM:
	case BITSTRUCT:
	case BITIDENT:
		return t;
	default:
		goto err;
	}
	err:
	errorposf(pos, "invalid declaration specifiers");
	return 0;
}

/* Declarator is what introduces names into the program. */
static CTy *
declarator(CTy *basety, char **name, Node **init) 
{
	CTy *t;

	while (tok->k == TOKCONST || tok->k == TOKVOLATILE)
		next();
	switch (tok->k) {
	case '*':
		next();
		basety = mkptr(basety);
		t = declarator(basety, name, init);
		return t;
	default:
		t = directdeclarator(basety, name);
		if (tok->k == '=') {
			if (!init)
				errorposf(&tok->pos, "unexpected initializer");
			next();
			*init = declinit(t);
		} else {
			if (init)
				*init = 0;
		}
		return t; 
	}

}

static CTy *
directdeclarator(CTy *basety, char **name) 
{
	CTy *ty, *stub;

	*name = 0;
	switch (tok->k) {
	case '(':
		expect('(');
		stub = xmalloc(sizeof(CTy));
		*stub = *basety;
		ty = declarator(stub, name, 0);
		expect(')');
		*stub = *declaratortail(basety);
		return ty;
	case TOKIDENT:
		if (name)
			*name = tok->v;
		next();
		return declaratortail(basety);
	default:
		if (!name)
			errorposf(&tok->pos, "expected ident or ( but got %s", tokktostr(tok->k));
		return declaratortail(basety);
	}
	errorf("unreachable");
	return 0;
}

static CTy *
declaratortail(CTy *basety)
{
	SrcPos *p;
	CTy    *t, *newt;
	Const  *c;
	
	t = basety;
	for(;;) {
		c = 0;
		switch (tok->k) {
		case '[':
			newt = newtype(CARR);
			newt->Arr.subty = t;
			newt->Arr.dim = -1;
			next();
			if (tok->k != ']') {
				p = &tok->pos;
				/* XXX check for int type? */
				c = constexpr();
				if (c->p)
					errorposf(p, "pointer derived constant in array size");
				newt->Arr.dim = c->v;
				newt->size = newt->Arr.dim * newt->Arr.subty->size;
			}
			newt->align = newt->Arr.subty->align;
			expect(']');
			t = newt;
			break;
		case '(':
			newt = newtype(CFUNC);
			newt->Func.rtype = basety;
			newt->Func.params = vec();
			next();
			params(newt);
			if (tok->k != ')')
				errorposf(&tok->pos, "expected valid parameter or )");
			next();
			t = newt;
			break;
		default:
			return t;
		}
	}
}

static CTy *
ptag()
{
	SrcPos *pos;
	char   *name;
	int     tkind;
	CTy    *namety, *bodyty;

	pos = &tok->pos;
	namety = 0;
	bodyty = 0;
	name = 0;
	switch (tok->k) {
	case TOKUNION:
	case TOKSTRUCT:
	case TOKENUM:
		tkind = tok->k;
		next();
		break;
	default:
		errorposf(pos, "expected a tag");
	}
	if (tok->k == TOKIDENT) {
		name = tok->v;
		next();
		namety = lookup(tags, name);
		if (namety) {
			switch (tkind) {
			case TOKUNION:
			case TOKSTRUCT:
				if (namety->t != CSTRUCT)
					errorposf(pos, "struct/union accessed by enum tag");
				if (namety->Struct.isunion != (tkind == TOKUNION))
					errorposf(pos, "struct/union accessed by wrong tag type");
				break;
			case TOKENUM:
				if (namety->t != CENUM)
					errorposf(pos, "enum tag accessed by struct or union");
				break;
			default:
				panic("internal error");
			}
		} else {
			switch (tkind) {
			case TOKUNION:
				namety = newtype(CSTRUCT);
				namety->Struct.isunion = 1;
				namety->incomplete = 1;
				break;
			case TOKSTRUCT:
				namety = newtype(CSTRUCT);
				namety->incomplete = 1;
				break;
			case TOKENUM:
				namety = newtype(CENUM);
				namety->incomplete = 1;
				break;
			default:
				panic("unreachable");
			}
			mapset(tags[nscopes - 1], name, namety);
		}
	}
	if (tok->k == '{' || !name) {
		switch (tkind) {
		case TOKUNION:
			bodyty = pstruct(1);
			break;
		case TOKSTRUCT:
			bodyty = pstruct(0);
			break;
		case TOKENUM:
			bodyty = penum();
			break;
		default:
			panic("unreachable");
		}
	}
	if (!name) {
		if (!bodyty)
			panic("internal error");
		return bodyty;
	}
	if (bodyty) {
		namety = mapget(tags[nscopes - 1], name);
		if (!namety) {
			mapset(tags[nscopes - 1], name, bodyty);
			return bodyty;
		}
		if (!namety->incomplete)
			errorposf(pos, "redefinition of tag %s", name);
		*namety = *bodyty;
		return namety;
	}
	return namety;
}

static CTy *
pstruct(int isunion)
{
	SrcPos *startpos, *p;
	CTy    *strct;
	char   *name;
	int     sclass;
	CTy    *t, *basety;

	strct = newtype(CSTRUCT);
	strct->align = 1; /* XXX zero sized structs? */
	strct->Struct.isunion = isunion;
	strct->Struct.exports = vec();
	strct->Struct.members = vec();
	
	startpos = &tok->pos;
	expect('{');
	while (tok->k != '}') {
		basety = declspecs(&sclass);
		for(;;) {
			p = &tok->pos;
			t = declarator(basety, &name, 0);
			if (tok->k == ':') {
				next();
				constexpr();
			}
			if (t->incomplete)
				errorposf(p, "incomplete type inside struct/union");
			addtostruct(strct, name, t);
			if (tok->k != ',')
				break;
			next();
		}
		expect(';');
	}
	expect('}');
	finalizestruct(startpos, strct);
	return strct;
}

static CTy *
penum()
{
	SrcPos *p;
	char   *name;
	CTy    *t;
	Const  *c;
	Sym    *s;
	int64   v;

	v = 0;
	t = newtype(CENUM);
	/* TODO: backend specific? */
	t->size = 4;
	t->align = 4;
	t->Enum.members = vec();
	expect('{');
	for(;;) {
		if (tok->k == '}')
			break;
		p = &tok->pos;
		name = tok->v;
		expect(TOKIDENT);
		if (tok->k == '=') {
			next();
			c = constexpr();
			if (c->p)
				errorposf(p, "pointer derived constant in enum");
			v = c->v;
		}
		s = defineenum(p, name, t, v);
		vecappend(t->Enum.members, s);
		if (tok->k != ',')
			break;
		next();
		v += 1;
	}
	expect('}');
	
	return t;
}


static void
funcbody()
{
	NameTy *nt;
	Sym    *sym;
	Goto   *go;
	int     i;

	pushscope();
	
	labels = map();
	gotos = vec();

	for (i = 0; i < curfunc->type->Func.params->len; i++) {
		nt = vecget(curfunc->type->Func.params, i);
		if (nt->name) {
			sym = definesym(curfunc->pos, SCAUTO, nt->name, nt->type, 0);
			sym->Local.isparam = 1;
			sym->Local.paramidx = i;
		}
	}

	block();
	popscope();

	for (i = 0 ; i < gotos->len ; i++) {
		go = vecget(gotos, i);
		if (!go->label->defined)
			errorposf(&go->pos, "goto target not defined");
	}
}


static void
pif (void)
{
	SrcPos *p;
	Node   *e;
	IRVal   cond;
	BasicBlock *iftruebb;
	BasicBlock *iffalsebb;
	BasicBlock *donebb;
	
	p = &tok->pos;
	expect(TOKIF);
	expect('(');
	e = expr();
	cond = compileexpr(e);
	expect(')');

	iftruebb = mkbasicblock();
	iffalsebb = mkbasicblock();
	donebb = mkbasicblock();

	bbterminate(currentbb, (Terminator){.op=Opcond, .v=cond, .label1=bbgetlabel(iftruebb), .label2=bbgetlabel(iffalsebb)});
	setcurbb(iftruebb);
	stmt();
	bbterminate(currentbb, (Terminator){.op=Opjmp, .label1=bbgetlabel(donebb)});
	setcurbb(iffalsebb);

	if (tok->k == TOKELSE) {
		expect(TOKELSE);
		stmt();
	}
	bbterminate(currentbb, (Terminator){.op=Opjmp, .label1=bbgetlabel(donebb)});
	setcurbb(donebb);
}

static void
pfor(void)
{
	SrcPos *p;
	Node   *n, *i, *c, *s;
	char   *lstart, *lstep, *lend;

	i = 0;
	c = 0;
	s = 0;
	lstart = newlabel(); 
	lstep = newlabel();
	lend = newlabel();
	p = &tok->pos;
	expect(TOKFOR);
	expect('(');
	if (tok->k == ';') {
		next();
	} else {
		i = expr();
		expect(';');
	}
	if (tok->k == ';') {
		next();
	} else {
		c = expr();
		expect(';');
	}
	if (tok->k != ')')
		s = expr();
	expect(')');
	pushcontbrk(lstep, lend);
	stmt();
	popcontbrk();
}

static void
pwhile(void)
{
	SrcPos *p;
	Node   *e;
	char   *lcont, *lbreak;
	
	lcont = newlabel();
	lbreak = newlabel();
	p = &tok->pos;	
	expect(TOKWHILE);
	expect('(');
	e = expr();
	expect(')');
	pushcontbrk(lcont, lbreak);
	stmt();
	popcontbrk();
}

static void
dowhile(void)
{
	SrcPos *p;
	Node   *e;
	char   *lstart, *lcont, *lbreak;
	
	lstart = newlabel();
	lcont = newlabel();
	lbreak = newlabel();
	p = &tok->pos;
	expect(TOKDO);
	pushcontbrk(lcont, lbreak);
	stmt();
	popcontbrk();
	expect(TOKWHILE);
	expect('(');
	e = expr();
	expect(')');
	expect(';');
}

static void
pswitch(void)
{
	SrcPos *p;
	Node   *e;
	char   *lbreak;
	
	lbreak = newlabel();
	p = &tok->pos;
	expect(TOKSWITCH);
	expect('(');
	e = expr();
	expect(')');
	pushbrk(lbreak);
	panic("unimplemented switch");
	pushswitch(NULL); // XXX TODO
	stmt();
	popswitch();
	popbrk();
}

static void
pgoto()
{
	Goto *go;

	go = xmalloc(sizeof(Goto));
	go->pos = tok->pos;
	expect(TOKGOTO);
	go->label = lookuplabel(tok->v);
	expect(TOKIDENT);
	expect(';');
	vecappend(gotos, go);
	bbterminate(currentbb, (Terminator){.op=Opjmp, .label1=go->label->backendlabel});
}

static int
istypename(char *n)
{
	Sym *sym;

	sym = lookup(syms, n);
	if (sym && sym->k == SYMTYPE)
		return 1;
	return 0;
}

static int
istypestart(Tok *t)
{
	switch(t->k) {
	case TOKENUM:
	case TOKSTRUCT:
	case TOKUNION:
	case TOKVOID:
	case TOKCHAR:
	case TOKSHORT:
	case TOKINT:
	case TOKLONG:
	case TOKSIGNED:
	case TOKUNSIGNED:
		return 1;
	case TOKIDENT:	
		return istypename(t->v);
	default:
		return 0;
	}
}

static int
isdeclstart(Tok *t)
{
	if (istypestart(t))
		return 1;
	switch (tok->k) {
	case TOKEXTERN:
	case TOKREGISTER:
	case TOKSTATIC:
	case TOKAUTO:
	case TOKCONST:
	case TOKVOLATILE:
		return 1;
	case TOKIDENT:
		return istypename(t->v);
	default:
		return 0;
	}
}

static void
declorstmt()
{
	if (isdeclstart(tok))
		decl();
	else
		stmt();
}

static void
stmt(void)
{
	Tok  *t;
	GotoLabel *label;

	if (tok->k == TOKIDENT && nexttok->k == ':') {
		t = tok;
		label = lookuplabel(t->v);
		next();
		next();
		if (label->defined)
			errorposf(&t->pos, "redefinition of label %s", t->v);
		label->defined = 1;
		bbterminate(currentbb, (Terminator){.op=Opjmp, .label1=label->backendlabel});
		setcurbb(mkbasicblock());
		vecappend(currentbb->labels, label->backendlabel);
		return;
	}
	switch (tok->k) {
	case TOKIF:
		pif ();
		return;
	case TOKFOR:
		pfor();
		return;
	case TOKWHILE:
		pwhile();
		return;
	case TOKDO:
		dowhile();
		return;
	case TOKRETURN:
		preturn();
		return;
	case TOKSWITCH:
		pswitch();
		return;
	case TOKCASE:
		pcase();
		return;
	case TOKDEFAULT:
		pdefault();
		return;
	case TOKBREAK:
		pbreak();
		return;
	case TOKCONTINUE:
		pcontinue();
		return;
	case TOKGOTO:
		pgoto();
		return;
	case '{':
		block();
		return;
	default:
		exprstmt();
		return;
	}
}

static int
compareinits(const void *lvoid, const void *rvoid)
{
	const InitMember *l, *r;
	
	l = *(void**)lvoid;
	r = *(void**)rvoid;
	if (l->offset < r->offset)
		return -1;
	if (l->offset > r->offset)
		return 1;
	return 0;
}

static void
checkinitoverlap(Node *n)
{
	InitMember *init, *nextinit;
	int i;

	qsort(n->Init.inits->d, n->Init.inits->len, sizeof(void*), compareinits);
	for(i = 0; i < n->Init.inits->len - 1; i++) {
		init = vecget(n->Init.inits, i);
		nextinit = vecget(n->Init.inits, i + 1);
		if (nextinit->offset < init->offset + init->n->type->size)
			errorposf(&init->n->pos, "fields in init overlaps with another field");
	}
}

static Node *
declarrayinit(CTy *t)
{
	InitMember *initmemb;
	Node   *n, *subinit;
	CTy    *subty;
	SrcPos *initpos, *selpos;
	Const  *arrayidx;
	int     i;
	int     idx;
	int     largestidx;
	
	subty = t->Arr.subty;
	initpos = &tok->pos;
	n = mknode(NINIT, initpos);
	n->type = t;
	n->Init.inits = vec();
	idx = 0;
	largestidx = 0;
	expect('{');
	for(;;) {
		if (tok->k == '}')
			break;
		if (tok->k == '[') {
			selpos = &tok->pos;
			expect('[');
			arrayidx = constexpr();
			expect(']');
			expect('=');
			if (arrayidx->p != 0)
				errorposf(selpos, "pointer derived constants not allowed in initializer selector");
			if (arrayidx->v < 0)
				errorposf(selpos, "negative initializer index not allowed");
			idx = arrayidx->v;
			if (largestidx < idx)
				largestidx = idx;
		}
		subinit = declinit(subty);
		/* Flatten nested inits */
		if (subinit->t == NINIT) {
			for(i = 0; i < subinit->Init.inits->len; i++) {
				initmemb = vecget(subinit->Init.inits, i);
				initmemb->offset = subty->size * idx + initmemb->offset;
				vecappend(n->Init.inits, initmemb);
			}
		} else {
			initmemb = xmalloc(sizeof(InitMember));
			initmemb->offset = subty->size * idx;
			initmemb->n = subinit;
			vecappend(n->Init.inits, initmemb);
		}
		idx += 1;
		if (largestidx < idx)
			largestidx = idx;
		if (tok->k != ',')
			break;
		next();
	}
	checkinitoverlap(n);
	expect('}');
	if (t->Arr.dim == -1)
		t->Arr.dim = largestidx;
	if (largestidx != t->Arr.dim)
		errorposf(initpos, "array initializer wrong size for type");
	return n;
}

static Node *
declstructinit(CTy *t)
{
	StructMember *structmember;
	InitMember   *initmemb;
	StructIter   it;
	Node         *n, *subinit;
	CTy          *subty;
	SrcPos       *initpos, *selpos;
	char         *selname;
	int          i, offset, neednext;
	
	initpos = &tok->pos;
	if (t->incomplete)
		errorposf(initpos, "cannot initialize an incomplete struct/union");
		
	n = mknode(NINIT, initpos);
	n->type = t;
	n->Init.inits = vec();
	
	initstructiter(&it, t);
	expect('{');
	neednext = 0;
	for(;;) {
		if (tok->k == '}')
			break;
		if (tok->k == '.') {
			neednext = 0;
			selpos = &tok->pos;
			expect('.');
			selname = tok->v;
			expect(TOKIDENT);
			expect('=');
			if (!getstructiter(&it, t, selname))
				errorposf(selpos, "struct has no member '%s'", selname);
		}
		if (neednext) {
			if (!structnext(&it))
				errorposf(&tok->pos, "end of struct already reached");
		}
		structwalk(&it, &structmember, &offset);
		if (!structmember)
			errorposf(initpos, "too many elements in struct initializer");
		subty = structmember->type;
		subinit = declinit(subty);
		/* Flatten nested inits */
		if (subinit->t == NINIT) {
			for(i = 0; i < subinit->Init.inits->len; i++) {
				initmemb = vecget(subinit->Init.inits, i);
				initmemb->offset += offset;
				vecappend(n->Init.inits, initmemb);
			}
		} else {
			initmemb = xmalloc(sizeof(InitMember));
			initmemb->offset = offset;
			initmemb->n = subinit;
			vecappend(n->Init.inits, initmemb);
		}
		if (tok->k != ',')
			break;
		next();
		neednext = 1;
	}
	checkinitoverlap(n);
	expect('}');
	return n;
}

static Node *
declinit(CTy *t)
{
	if (isarray(t) && tok->k == '{') 
		return declarrayinit(t);
	if (isstruct(t)  && tok->k == '{') 
		return declstructinit(t);
	return assignexpr();
}

static void
exprstmt(void)
{
	Node *e;

	if (tok->k == ';') {
		next();
		return;
	}
	e = expr();
	compileexpr(e);
	expect(';');
}

static void
preturn(void)
{   
	Node *e;
	IRVal ret;

	expect(TOKRETURN);
	if (tok->k != ';') {
		e = expr();
		e = mkcast(&e->pos, e, curfunc->type->Func.rtype);
		ret = compileexpr(e);
		bbterminate(currentbb, (Terminator){.op=Opret, .v=ret});
	}
	expect(';');
}

static void
pcontinue(void)
{
	SrcPos *pos;
	char   *l;
	
	pos = &tok->pos;
	l = curcont();
	if (!l)
		errorposf(pos, "continue without parent statement");
	expect(TOKCONTINUE);
	expect(';');
}

static void
pbreak(void)
{
	SrcPos *pos;
	char   *l;
	
	pos = &tok->pos;
	l = curbrk();
	if (!l)
		errorposf(pos, "break without parent statement");
	expect(TOKBREAK);
	expect(';');
}

static void
pdefault(void)
{
	SrcPos *pos;
	Node   *s;
	char   *l;

	pos = &tok->pos;
	l = newlabel();
	panic("unimplemented pdefault");
	/*
	s = curswitch();
	if (s->Switch.ldefault)
		errorposf(pos, "switch already has default");
	s->Switch.ldefault = l;
	*/
	expect(TOKDEFAULT);
	expect(':');
	stmt();
}

static void
pcase(void)
{
	SrcPos *pos;
	Const  *c;

	panic("unimplemented pcase");
	/*
	pos = &tok->pos;
	s = curswitch();
	expect(TOKCASE);
	c = constexpr();
	if (c->p)
		errorposf(pos, "case cannot have pointer derived constant");
	n->Case.cond = c->v;
	expect(':');
	stmt();
	vecappend(s->Switch.cases, n);
	*/
}

static void
block(void)
{
	pushscope();
	expect('{');
	while(tok->k != '}' && tok->k != TOKEOF)
		declorstmt();
	expect('}');
	popscope();
}


static Node *
mknode(int type, SrcPos *p)
{
	Node *n;

	n = xmalloc(sizeof(Node));
	n->pos = *p;
	n->t = type;
	return n;
}

static int
islval(Node *n)
{
	switch (n->t) {
	case NUNOP:
		if (n->Unop.op == '*')
			return 1;
		return 0;
	case NIDENT:
	case NIDX:
	case NSEL:
	case NINIT:
		return 1;
	default:
		return 0;
	}
}

static Node *
mkincdec(SrcPos *p, int op, int post, Node *operand)
{
	Node *n;

	if (!islval(operand))
		errorposf(&operand->pos, "++ and -- expects an lvalue");
	n = mknode(NINCDEC, p);
	n->Incdec.op = op;
	n->Incdec.post = post;
	n->Incdec.operand = operand;
	n->type = operand->type;
	return n;
}

static Node *
mkptradd(SrcPos *p, Node *ptr, Node *offset)
{
	Node *n;
	if (!isptr(ptr->type))
		panic("internal error");
	if (!isitype(offset->type))
		errorposf(&offset->pos, "addition with a pointer requires an integer type");
	n = mknode(NPTRADD, p);
	n->Ptradd.ptr = ptr;
	n->Ptradd.offset = offset;
	n->type = ptr->type;
	return n;
}

static Node *
mkbinop(SrcPos *p, int op, Node *l, Node *r)
{
	Node *n;
	CTy  *t;
	
	t = 0;
	if (op == '+') {
		if (isptr(l->type))
			return mkptradd(p, l, r);
		if (isptr(r->type))
			return mkptradd(p, r, l);
	}
	
	if (!isptr(l->type))
		l = ipromote(l);
	if (!isptr(r->type))
		r = ipromote(r);
	if (!isptr(l->type) && !isptr(r->type))
		t = usualarithconv(&l, &r);
	/* Other relationals? */
	if (op == TOKEQL || op == TOKNEQ)
		t = cint;
	n = mknode(NBINOP, p);
	n->Binop.op = op;
	n->Binop.l = l;
	n->Binop.r = r;
	n->type = t;
	return n;
}

static Node *
mkassign(SrcPos *p, int op, Node *l, Node *r)
{
	Node *n;
	CTy  *t;

	if (!islval(l))
		errorposf(&l->pos, "assign expects an lvalue");
	r = mkcast(p, r, l->type);
	t = l->type;
	n = mknode(NASSIGN, p);
	switch (op) {
	case '=':
		n->Assign.op = '=';
		break;
	case TOKADDASS:
		n->Assign.op = '+';
		break;
	case TOKSUBASS:
		n->Assign.op = '-';
		break;
	case TOKORASS:
		n->Assign.op = '|';
		break;
	case TOKANDASS:
		n->Assign.op = '&';
		break;
	case TOKMULASS:
		n->Assign.op = '*';
		break;
	default:
		panic("mkassign");
	}
	n->Assign.l = l;
	n->Assign.r = r;
	n->type = t;
	return n;
}

static Node *
mkunop(SrcPos *p, int op, Node *o)
{
	Node *n;
	
	n = mknode(NUNOP, p);
	n->Unop.op = op;
	switch (op) {
	case '&':
		if (!islval(o))
			errorposf(&o->pos, "& expects an lvalue");
		n->type = mkptr(o->type);
		break;
	case '*':
		if (!isptr(o->type))
			errorposf(&o->pos, "cannot deref non pointer");
		n->type = o->type->Ptr.subty;
		break;
	default:
		if (isitype(o->type))
			o = ipromote(o);
		n->type = o->type;
		break;
	}
	n->Unop.operand = o;
	return n;
}

static Node *
mkcast(SrcPos *p, Node *o, CTy *to)
{
	Node *n;
	
	if (sametype(o->type, to))
		return o;
	n = mknode(NCAST, p);
	n->type = to;
	n->Cast.operand = o;
	return n;
}

static Node *
ipromote(Node *n)
{
	if (!isitype(n->type))
		errorposf(&n->pos, "internal error - ipromote expects itype got %d", n->type->t);
	switch (n->type->Prim.type) {
	case PRIMCHAR:
	case PRIMSHORT:
		if (n->type->Prim.issigned)
			return mkcast(&n->pos, n, cint);
		else
			return mkcast(&n->pos, n, cuint);
	}
	return n;
}

static CTy *
usualarithconv(Node **a, Node **b)
{   
	Node **large, **small;
	CTy   *t;

	if (!isarithtype((*a)->type) || !isarithtype((*b)->type))
		panic("internal error\n");
	if (convrank((*a)->type) < convrank((*b)->type)) {
		large = a;
		small = b;
	} else {
		large = b;
		small = a;
	}
	if (isftype((*large)->type)) {
		*small = mkcast(&(*small)->pos, *small, (*large)->type);
		return (*large)->type;
	}
	*large = ipromote(*large);
	*small = ipromote(*small);
	if (sametype((*large)->type, (*small)->type))
		return (*large)->type;
	if ((*large)->type->Prim.issigned == (*small)->type->Prim.issigned ) {
		*small = mkcast(&(*small)->pos, *small, (*large)->type);
		return (*large)->type;
	}
	if (!(*large)->type->Prim.issigned) {
		*small = mkcast(&(*small)->pos, *small, (*large)->type);
		return (*large)->type;
	}
	if ((*large)->type->Prim.issigned && canrepresent((*large)->type, (*small)->type)) {
		*small = mkcast(&(*small)->pos, *small, (*large)->type);
		return (*large)->type;
	}
	t = xmalloc(sizeof(CTy));
	*t = *((*large)->type);
	t->Prim.issigned = 0;
	*large = mkcast(&(*large)->pos, *large, t);
	*small = mkcast(&(*small)->pos, *small, t);
	return t;
}

static Node *
expr(void)
{
	SrcPos *p;
	Vec    *v;
	Node   *n, *last;

	p = &tok->pos;
	n = assignexpr();
	last = n;
	if (tok->k == ',') {
		v = vec();
		vecappend(v, n);
		while(tok->k == ',') {
			next();
			last = assignexpr();
			vecappend(v, last);
		}
		n = mknode(NCOMMA, p);
		n->Comma.exprs = v;
		n->type = last->type;
	}
	return n;
}

static int
isassignop(Tokkind k)
{
	switch (k) {
	case '=':
	case TOKADDASS:
	case TOKSUBASS:
	case TOKMULASS:
	case TOKDIVASS:
	case TOKMODASS:
	case TOKORASS:
	case TOKANDASS:
		return 1;
	default:
		return 0;
	}
}

static Node *
assignexpr(void)
{
	Tok  *t;
	Node *l, *r;

	l = condexpr();
	if (isassignop(tok->k)) {
		t = tok;
		next();
		r = assignexpr();
		l = mkassign(&t->pos, t->k, l, r);
	}
	return l;
}

static Const *
constexpr(void)
{
	/*
	Node  *n;
	*/
	Const *c;

	c = xmalloc(sizeof(constexpr));
	c->p = 0;
	c->v = 0;

	/*
	n = condexpr();
	c = foldexpr(n);
	if (!c)
		errorposf(&n->pos, "not a constant expression");
	*/
	return c;
}

/* Aka Ternary operator. */
static Node *
condexpr(void)
{
	Node *n, *c, *t, *f;

	c = logorexpr();
	if (tok->k != '?')
		return c;
	next();
	t = expr();
	expect(':');
	f = condexpr();
	n = mknode(NCOND, &tok->pos);
	n->Cond.cond = c;
	n->Cond.iftrue = t;
	n->Cond.iffalse = f;
	/* TODO: what are the limitations? */
	if (!sametype(t->type, f->type))
		errorposf(&n->pos, "both cases of ? must be same type.");
	n->type = t->type;
	return n;
}

static Node *
logorexpr(void)
{
	Tok  *t;
	Node *l, *r;

	l = logandexpr();
	while(tok->k == TOKLOR) {
		t = tok;
		next();
		r = logandexpr();
		l = mkbinop(&t->pos, t->k, l, r);
	}
	return l;
}

static Node *
logandexpr(void)
{
	Tok  *t;
	Node *l, *r;

	l = orexpr();
	while(tok->k == TOKLAND) {
		t = tok;
		next();
		r = orexpr();
		l = mkbinop(&t->pos, t->k, l, r);
	}
	return l;
}

static Node *
orexpr(void)
{
	Tok  *t;
	Node *l, *r;

	l = xorexpr();
	while(tok->k == '|') {
		t = tok;
		next();
		r = xorexpr();
		l = mkbinop(&t->pos, t->k, l, r);
	}
	return l;
}

static Node *
xorexpr(void)
{
	Tok  *t;
	Node *l, *r;

	l = andexpr();
	while(tok->k == '^') {
		t = tok;
		next();
		r = andexpr();
		l = mkbinop(&t->pos, t->k, l, r);
	}
	return l;
}

static Node *
andexpr(void) 
{
	Tok  *t;
	Node *l, *r;

	l = eqlexpr();
	while(tok->k == '&') {
		t = tok;
		next();
		r = eqlexpr();
		l = mkbinop(&t->pos, t->k, l, r);
	}
	return l;
}

static Node *
eqlexpr(void)
{
	Tok  *t;
	Node *l, *r;

	l = relexpr();
	while(tok->k == TOKEQL || tok->k == TOKNEQ) {
		t = tok;
		next();
		r = relexpr();
		l = mkbinop(&t->pos, t->k, l, r);
	}
	return l;
}

static Node *
relexpr(void)
{
	Tok  *t;
	Node *l, *r;

	l = shiftexpr();
	while(tok->k == '>' || tok->k == '<' 
		  || tok->k == TOKLEQ || tok->k == TOKGEQ) {
		t = tok;
		next();
		r = shiftexpr();
		l = mkbinop(&t->pos, t->k, l, r);
	}
	return l;
}

static Node *
shiftexpr(void)
{
	Tok  *t;
	Node *l, *r;

	l = addexpr();
	while(tok->k == TOKSHL || tok->k == TOKSHR) {
		t = tok;
		next();
		r = addexpr();
		l = mkbinop(&t->pos, t->k, l, r);
	}
	return l;
}

static Node *
addexpr(void)
{
	Tok  *t;
	Node *l, *r;

	l = mulexpr();
	while(tok->k == '+' || tok->k == '-'	) {
		t = tok;
		next();
		r = mulexpr();
		l = mkbinop(&t->pos, t->k, l, r);
	}
	return l;
}

static Node *
mulexpr(void)
{
	Tok  *t;
	Node *l, *r;
	
	l = castexpr();
	while(tok->k == '*' || tok->k == '/' || tok->k == '%') {
		t = tok;
		next();
		r = castexpr();
		l = mkbinop(&t->pos, t->k, l, r);
	}
	return l;
}

static Node *
castexpr(void)
{
	Tok  *t;
	Node *o;
	CTy  *ty;
	
	if (tok->k == '(' && istypestart(nexttok)) {
		t = tok;
		expect('(');
		ty = typename();
		expect(')');
		if (tok->k == '{') {
			o = declinit(ty);
			return o;
		}
		o = unaryexpr();
		return mkcast(&t->pos, o, ty);
	}
	return unaryexpr();
}

static CTy *
typename(void)
{
	int   sclass;
	CTy  *t;
	char *name;
	
	t = declspecs(&sclass);
	t = declarator(t, &name, 0);
	return t;
}

static Node *
unaryexpr(void)
{
	Tok  *t;
	CTy  *ty;
	Node *n;

	switch (tok->k) {
	case TOKINC:
	case TOKDEC:
		t = tok;
		next();
		n = unaryexpr();
		return mkincdec(&t->pos, t->k, 0, n);
	case '*':
	case '&':
	case '-':
	case '!':
	case '~':
		t = tok;
		next();
		return mkunop(&t->pos, t->k, castexpr());
	case TOKSIZEOF:
		n = mknode(NSIZEOF, &tok->pos);
		next();
		if (tok->k == '(' && istypestart(nexttok)) {
			expect('(');
			ty = typename();
			expect(')');
		} else {
			ty = unaryexpr()->type;
		}
		n->Sizeof.type = ty;
		n->type = cint;
		return n;
	default:
		;
	}
	return postexpr();
}

static Node *
call(Node *funclike)
{
	Node   *n;
	CTy    *fty;
	SrcPos *pos ;

	pos = &tok->pos;
	expect('(');
	n = mknode(NCALL, &tok->pos);
	n->Call.funclike = funclike;
	n->Call.args = vec();
	if (isfunc(funclike->type))
		fty = funclike->type;
	else if (isfuncptr(funclike->type))
		fty = funclike->type->Ptr.subty;
	else
		errorposf(pos, "cannot call non function");
	n->type = fty->Func.rtype;
	if (tok->k != ')') {
		for(;;) {
			vecappend(n->Call.args, assignexpr());
			if (tok->k != ',') {
				break;
			}
			next();
		}
	}
	expect(')');
	if (n->Call.args->len < fty->Func.params->len)
		errorposf(pos, "function called with too few args");
	if (n->Call.args->len > fty->Func.params->len && !fty->Func.isvararg)
		errorposf(pos, "function called with too many args");
	return n;
}

static Node *
postexpr(void)
{
	int   done;
	Tok  *t;
	Node *n1, *n2, *n3;

	n1 = primaryexpr();
	done = 0;
	while(!done) {
		switch (tok->k) {
		case '[':
			t = tok;
			next();
			n2 = expr();
			expect(']');
			n3 = mknode(NIDX, &t->pos);
			if (isptr(n1->type))
				n3->type = n1->type->Ptr.subty;
			else if (isarray(n1->type))
				n3->type = n1->type->Arr.subty;
			else
				errorposf(&t->pos, "can only index an array or pointer");
			n3->Idx.idx = n2;
			n3->Idx.operand = n1;
			n1 = n3;
			break;
		case '.':
			if (!isstruct(n1->type))
				errorposf(&tok->pos, "expected a struct");
			if (n1->type->incomplete)
				errorposf(&tok->pos, "selector on incomplete type");
			n2 = mknode(NSEL, &tok->pos);
			next();
			n2->Sel.name = tok->v;
			n2->Sel.operand = n1;
			n2->type = structtypefromname(n1->type, tok->v);
			if (!n2->type)
				errorposf(&tok->pos, "struct has no member %s", tok->v);
			expect(TOKIDENT);
			n1 = n2;
			break;
		case TOKARROW:
			if (!(isptr(n1->type) && isstruct(n1->type->Ptr.subty)))
				errorposf(&tok->pos, "expected a struct pointer");
			if (n1->type->Ptr.subty->incomplete)
				errorposf(&tok->pos, "selector on incomplete type");
			n2 = mknode(NSEL, &tok->pos);
			next();
			n2->Sel.name = tok->v;
			n2->Sel.operand = n1;
			n2->Sel.arrow = 1;
			n2->type = structtypefromname(n1->type->Ptr.subty, tok->v);
			if (!n2->type)
				errorposf(&tok->pos, "struct pointer has no member %s", tok->v);
			expect(TOKIDENT);
			n1 = n2;
			break;
		case '(':
			n1 = call(n1);
			break;
		case TOKINC:
			n1 = mkincdec(&tok->pos, TOKINC, 1, n1);
			next();
			break;
		case TOKDEC:
			n1 = mkincdec(&tok->pos, TOKDEC, 1, n1);
			next();
			break;
		default:
			done = 1;
		}
	}
	return n1;
}

static Node *
primaryexpr(void) 
{
	Sym  *sym;
	Node *n;
	
	switch (tok->k) {
	case TOKIDENT:
		if (strcmp(tok->v, "__builtin_va_start") == 0)
			return vastart();
		sym = lookup(syms, tok->v);
		if (!sym)
			errorposf(&tok->pos, "undefined symbol %s", tok->v);
		n = mknode(NIDENT, &tok->pos);
		n->Ident.sym = sym;
		n->type = sym->type;
		next();
		return n;
	case TOKNUM:
		n = mknode(NNUM, &tok->pos);
		n->Num.v = atoll(tok->v);
		n->type = cint;
		next();
		return n;
	case TOKCHARLIT:
		/* XXX it seems wrong to do this here, also table is better */
		n = mknode(NNUM, &tok->pos);
		if (strcmp(tok->v, "'\\n'") == 0) {
			n->Num.v = '\n';
		} else if (strcmp(tok->v, "'\\\\'") == 0) {
			n->Num.v = '\\';
		} else if (strcmp(tok->v, "'\\''") == 0) {
			n->Num.v = '\'';
		} else if (strcmp(tok->v, "'\\r'") == 0) {
			n->Num.v = '\r';
		} else if (strcmp(tok->v, "'\\t'") == 0) {
			n->Num.v = '\t';
		}  else if (tok->v[1] == '\\') {
			errorposf(&tok->pos, "unknown escape code");
		} else {
			n->Num.v = tok->v[1];
		}
		n->type = cint;
		next();
		return n;
	case TOKSTR:
		n = mknode(NSTR, &tok->pos);
		n->Str.v = tok->v;
		n->type = mkptr(cchar);
		next();
		return n;
	case '(':
		next();
		n = expr();
		expect(')');
		return n;
	default:
		errorposf(&tok->pos, "expected an ident, constant, string or (");
	}
	errorf("unreachable.");
	return 0;
}

static Node *
vastart()
{
	Node *n, *valist, *param;
	
	n = mknode(NBUILTIN, &tok->pos);
	expect(TOKIDENT);
	expect('(');
	valist = assignexpr();
	expect(',');
	param = assignexpr();
	expect(')');
	n->type = cvoid;
	n->Builtin.t = BUILTIN_VASTART;
	n->Builtin.Vastart.param = param;
	n->Builtin.Vastart.valist = valist;
	if (param->t != NIDENT)
		errorposf(&n->pos, "expected an identifer in va_start");
	if (param->Ident.sym->k != SYMLOCAL 
	   || !param->Ident.sym->Local.isparam)
		errorposf(&n->pos, "expected a parameter symbol in va_start");
	return n;
}

void
compile()
{
	int i;
	Sym *sym;

	switchdepth = 0;
	brkdepth = 0;
	contdepth = 0;
	nscopes = 0;
	tentativesyms = vec();
	pushscope();
	next();
	next();

	beginmodule();
	
	while (tok->k != TOKEOF)
		decl();
	
	for(i = 0; i < tentativesyms->len; i++) {
		sym = vecget(tentativesyms, i);
		emitsym(sym);
	}
	
	endmodule();
}

static IRVal
compileexpr(Node *n)
{
	switch(n->t){
	/*
	case NCOMMA:
		comma(n);
		break;
	case NCAST:
		cast(n);
		break;
	case NSTR:
		str(n);
		break;
	*/
	case NSIZEOF:
		if (n->Sizeof.type->incomplete)
			errorposf(&n->pos, "cannot use incomplete type in sizeof");

		return (IRVal){.kind=IRConst, .irtype=ctype2irtype(n->type), .v=n->Sizeof.type->size};
	case NNUM:
		return (IRVal){.kind=IRConst, .irtype=ctype2irtype(n->type), .v=n->Num.v};
	/*
	case NIDENT:
		ident(n);
		break;
	case NUNOP:
		unop(n);
		break;
	case NASSIGN:
		assign(n);
		break;
	*/
	case NBINOP:
		return compilebinop(n);
	/*
	case NIDX:
		idx(n);
		break;
	case NSEL:
		sel(n);
		break;
	case NCOND:
		cond(n);
		break;
	case NCALL:
		call(n);
		break;
	case NPTRADD:
		ptradd(n);
		break;
	case NINCDEC:
		incdec(n);
		break;
	case NBUILTIN:
		switch(n->Builtin.t) {
		case BUILTIN_VASTART:
			vastart(n);
			break;
		default:
			errorposf(&n->pos, "unimplemented builtin");
		}
		break;
	*/
	default:
		panic("unimplemented compileexpr for node at %s:%d:%d\n", n->pos.file, n->pos.line, n->pos.col);
	}
}

static IRVal
compilebinop(Node *n)
{
	IRVal  res, l, r;

	if (n->Binop.op == TOKLAND || n->Binop.op == TOKLOR) {
		panic("shortcircuit unimplemented");
		// shortcircuit(n);
		// return;
	}
	l = compileexpr(n->Binop.l);
	r = compileexpr(n->Binop.r);

	res = nextvreg(ctype2irtype(n->type));
	
	switch (n->Binop.op) {
	case '+':
		bbappend(currentbb, (Instruction){.op=Opadd, .a=res, .b=l, .c=r});
		break;
	case '-':
		bbappend(currentbb, (Instruction){.op=Opsub, .a=res, .b=l, .c=r});
		break;
	case '*':
		bbappend(currentbb, (Instruction){.op=Opmul, .a=res, .b=l, .c=r});
		break;
	case '/':
		bbappend(currentbb, (Instruction){.op=Opdiv, .a=res, .b=l, .c=r});
		break;
	case '%':
		bbappend(currentbb, (Instruction){.op=Oprem, .a=res, .b=l, .c=r});
		break;
	case '|':
		bbappend(currentbb, (Instruction){.op=Opbor, .a=res, .b=l, .c=r});
		break;
	case '&':
		bbappend(currentbb, (Instruction){.op=Opband, .a=res, .b=l, .c=r});
		break;
	case '^':
		bbappend(currentbb, (Instruction){.op=Opbxor, .a=res, .b=l, .c=r});
		break;
	case TOKSHR:
	case TOKSHL:
	case TOKEQL:
	case TOKNEQ:
	case TOKGEQ:
	case TOKLEQ:
	case '>':
	case '<':
	default:
		errorf("unimplemented binop %d\n", n->Binop.op);
	}

	return res;
}


