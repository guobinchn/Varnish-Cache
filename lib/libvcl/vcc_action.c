/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file parses the real action of the VCL code, the procedure
 * statements which do the actual work.
 */

#include "config.h"

#include <string.h>

#include "vcc_compile.h"

/*--------------------------------------------------------------------*/

static void
parse_call(struct vcc *tl)
{

	vcc_NextToken(tl);
	ExpectErr(tl, ID);
	vcc_AddCall(tl, tl->t);
	vcc_AddRef(tl, tl->t, SYM_SUB);
	Fb(tl, 1, "if (VGC_function_%.*s(req))\n", PF(tl->t));
	Fb(tl, 1, "\treturn (1);\n");
	vcc_NextToken(tl);
	return;
}

/*--------------------------------------------------------------------*/

static void
parse_error(struct vcc *tl)
{

	vcc_NextToken(tl);
	Fb(tl, 1, "VRT_error(req,\n");
	if (tl->t->tok == '(') {
		vcc_NextToken(tl);
		vcc_Expr(tl, INT);
		if (tl->t->tok == ',') {
			Fb(tl, 1, ",\n");
			vcc_NextToken(tl);
			vcc_Expr(tl, STRING);
		} else
			Fb(tl, 1, ", 0\n");
		SkipToken(tl, ')');
	} else {
		vcc_Expr(tl, INT);
		if (tl->t->tok != ';') {
			Fb(tl, 1, ",\n");
			vcc_Expr(tl, STRING);
		} else
			Fb(tl, 1, ", 0\n");
	}
	Fb(tl, 1, ");\n");
	Fb(tl, 1, "VRT_done(req, VCL_RET_ERROR);\n");
}

/*--------------------------------------------------------------------*/

static const struct arith {
	enum var_type		type;
	unsigned		oper;
	enum var_type		want;
} arith[] = {
	{ INT,		T_INCR,		INT },
	{ INT,		T_DECR,		INT },
	{ INT,		T_MUL,		INT },
	{ INT,		T_DIV,		INT },
	{ INT,		'=',		INT },
	{ INT,		0,		INT },
	{ TIME,		T_INCR,		DURATION },
	{ TIME,		T_DECR,		DURATION },
	{ TIME,		T_MUL,		REAL },
	{ TIME,		T_DIV,		REAL },
	{ TIME,		'=',		TIME },
	{ TIME,		0,		TIME },
	{ DURATION,	T_INCR,		DURATION },
	{ DURATION,	T_DECR,		DURATION },
	{ DURATION,	T_MUL,		REAL },
	{ DURATION,	T_DIV,		REAL },
	{ DURATION,	'=',		DURATION },
	{ DURATION,	0,		DURATION },
	{ VOID,		'=',		VOID }
};

static void
parse_set(struct vcc *tl)
{
	const struct var *vp;
	const struct arith *ap;
	enum var_type fmt;

	vcc_NextToken(tl);
	ExpectErr(tl, ID);
	vp = vcc_FindVar(tl, tl->t, 1, "cannot be set");
	ERRCHK(tl);
	assert(vp != NULL);
	Fb(tl, 1, "%s\n", vp->lname);
	tl->indent += INDENT;
	vcc_NextToken(tl);
	fmt = vp->fmt;
	for (ap = arith; ap->type != VOID; ap++) {
		if (ap->type != fmt)
			continue;
		if (ap->oper != tl->t->tok)
			continue;
		if (ap->oper != '=')
			Fb(tl, 1, "%s %c ", vp->rname, *tl->t->b);
		vcc_NextToken(tl);
		fmt = ap->want;
		break;
	}
	if (ap->type == VOID)
		SkipToken(tl, ap->oper);
	if (fmt == HEADER) {
		vcc_Expr(tl, STRING_LIST);
	} else if (fmt == STRING) {
		vcc_Expr(tl, STRING_LIST);
	} else {
		vcc_Expr(tl, fmt);
	}
	tl->indent -= INDENT;
	Fb(tl, 1, ");\n");
}

/*--------------------------------------------------------------------*/

static void
parse_unset(struct vcc *tl)
{
	const struct var *vp;

	/* XXX: Wrong, should use VCC_Expr(HEADER) */
	vcc_NextToken(tl);
	ExpectErr(tl, ID);
	vp = vcc_FindVar(tl, tl->t, 1, "cannot be unset");
	ERRCHK(tl);
	assert(vp != NULL);
	if (vp->fmt != HEADER) {
		VSB_printf(tl->sb,
		    "Only http header variables can be unset.\n");
		vcc_ErrWhere(tl, tl->t);
		return;
	}
	ERRCHK(tl);
	Fb(tl, 1, "%svrt_magic_string_unset);\n", vp->lname);
	vcc_NextToken(tl);
}

/*--------------------------------------------------------------------*/

static void
parse_new(struct vcc *tl)
{
	struct symbol *sy1, *sy2, *sy3;
	const char *p, *s_obj, *s_init, *s_struct, *s_fini;
	char buf1[128];
	char buf2[128];

	vcc_NextToken(tl);
	ExpectErr(tl, ID);
	sy1 = VCC_FindSymbol(tl, tl->t, SYM_NONE);
	XXXAZ(sy1);

	sy1 = VCC_AddSymbolTok(tl, tl->t, SYM_NONE);	// XXX: NONE ?
	XXXAN(sy1);
	vcc_NextToken(tl);

	ExpectErr(tl, '=');
	vcc_NextToken(tl);

	ExpectErr(tl, ID);
	sy2 = VCC_FindSymbol(tl, tl->t, SYM_OBJECT);
	XXXAN(sy2);

	/*lint -save -e448 */
	/* Split the first three args */
	p = sy2->args;
	s_obj = p;
	p += strlen(p) + 1;
	s_init = p;
	while (p[0] != '\0' || p[1] != '\0')
		p++;
	p += 2;
	s_struct = p;
	p += strlen(p) + 1;
	s_fini = p + strlen(p) + 1;
	while (p[0] != '\0' || p[1] != '\0')
		p++;
	p += 2;

	Fh(tl, 0, "static %s *%s;\n\n", s_struct, sy1->name);

	vcc_NextToken(tl);

	bprintf(buf1, ", &%s, \"%s\"", sy1->name, sy1->name);
	vcc_Eval_Func(tl, s_init, buf1, "ASDF", s_init + strlen(s_init) + 1);
	Fd(tl, 0, "\t%s((struct req*)0, &%s);\n", s_fini, sy1->name);
	ExpectErr(tl, ';');

	bprintf(buf1, ", %s", sy1->name);
	/* Split the methods from the args */
	while (*p != '\0') {
		p += strlen(s_obj);
		bprintf(buf2, "%s%s", sy1->name, p);
		sy3 = VCC_AddSymbolStr(tl, buf2, SYM_FUNC);
		AN(sy3);
		sy3->eval = vcc_Eval_SymFunc;
		p += strlen(p) + 1;
		sy3->cfunc = p;
		p += strlen(p) + 1;

		/* Functions which return VOID are procedures */
		if (!memcmp(p, "VOID\0", 5))
			sy3->kind = SYM_PROC;

		sy3->args = p;
		sy3->extra = TlDup(tl, buf1);
		while (p[0] != '\0' || p[1] != '\0')
			p++;
		p += 2;
	}
	/*lint -restore */
}

/*--------------------------------------------------------------------*/

static void
parse_ban(struct vcc *tl)
{

	vcc_NextToken(tl);

	ExpectErr(tl, '(');
	vcc_NextToken(tl);

	Fb(tl, 1, "VRT_ban_string(req, ");
	vcc_Expr(tl, STRING);
	ERRCHK(tl);
	Fb(tl, 0, ");\n");

	ExpectErr(tl, ')');
	vcc_NextToken(tl);
}

/*--------------------------------------------------------------------*/

static void
parse_new_syntax(struct vcc *tl)
{

	VSB_printf(tl->sb, "Please change \"%.*s\" to \"return(%.*s)\".\n",
	    PF(tl->t), PF(tl->t));
	vcc_ErrWhere(tl, tl->t);
}

/*--------------------------------------------------------------------*/

static void
parse_hash_data(struct vcc *tl)
{
	vcc_NextToken(tl);
	SkipToken(tl, '(');

	Fb(tl, 1, "VRT_hashdata(req, ");
	vcc_Expr(tl, STRING_LIST);
	ERRCHK(tl);
	Fb(tl, 0, ");\n");
	SkipToken(tl, ')');
}

/*--------------------------------------------------------------------*/

static void
parse_return(struct vcc *tl)
{
	int retval = 0;

	vcc_NextToken(tl);
	ExpectErr(tl, '(');
	vcc_NextToken(tl);
	ExpectErr(tl, ID);

#define VCL_RET_MAC(l, U, B)						\
	do {								\
		if (vcc_IdIs(tl->t, #l)) {				\
			Fb(tl, 1, "VRT_done(req, VCL_RET_" #U ");\n");	\
			vcc_ProcAction(tl->curproc, VCL_RET_##U, tl->t);\
			retval = 1;					\
		}							\
	} while (0);
#include "tbl/vcl_returns.h"
#undef VCL_RET_MAC
	if (!retval) {
		VSB_printf(tl->sb, "Expected return action name.\n");
		vcc_ErrWhere(tl, tl->t);
		ERRCHK(tl);
	}
	vcc_NextToken(tl);
	ExpectErr(tl, ')');
	vcc_NextToken(tl);
}

/*--------------------------------------------------------------------*/

static void
parse_rollback(struct vcc *tl)
{

	vcc_NextToken(tl);
	Fb(tl, 1, "VRT_Rollback(req);\n");
}

/*--------------------------------------------------------------------*/

static void
parse_purge(struct vcc *tl)
{

	vcc_NextToken(tl);
	Fb(tl, 1, "VRT_purge(req, 0, 0);\n");
}

/*--------------------------------------------------------------------*/

static void
parse_synthetic(struct vcc *tl)
{
	vcc_NextToken(tl);

	Fb(tl, 1, "VRT_synth_page(req, 0, ");
	vcc_Expr(tl, STRING_LIST);
	ERRCHK(tl);
	Fb(tl, 0, ");\n");
}

/*--------------------------------------------------------------------*/

typedef void action_f(struct vcc *tl);

static struct action_table {
	const char		*name;
	action_f		*func;
	unsigned		bitmask;
} action_table[] = {
	{ "error",		parse_error,
	    VCL_MET_RECV | VCL_MET_PIPE | VCL_MET_PASS | VCL_MET_HASH |
            VCL_MET_MISS | VCL_MET_HIT | VCL_MET_RESPONSE
	},

#define VCL_RET_MAC(l, U, B)						\
	{ #l,			parse_new_syntax },
#include "tbl/vcl_returns.h"
#undef VCL_RET_MAC

	/* Keep list sorted from here */
	{ "call",		parse_call },
	{ "hash_data",		parse_hash_data, VCL_MET_HASH },
	{ "ban",		parse_ban },
	{ "remove",		parse_unset }, /* backward compatibility */
	{ "return",		parse_return },
	{ "rollback",		parse_rollback },
	{ "set",		parse_set },
	{ "synthetic",		parse_synthetic, VCL_MET_ERROR },
	{ "unset",		parse_unset },
	{ "purge",		parse_purge, VCL_MET_MISS | VCL_MET_HIT },
	{ "new",		parse_new, VCL_MET_INIT},
	{ NULL,			NULL }
};

int
vcc_ParseAction(struct vcc *tl)
{
	struct token *at;
	struct action_table *atp;
	const struct symbol *sym;

	at = tl->t;
	assert(at->tok == ID);
	for(atp = action_table; atp->name != NULL; atp++) {
		if (vcc_IdIs(at, atp->name)) {
			if (atp->bitmask != 0)
				vcc_AddUses(tl, at, atp->bitmask,
				    "not a valid action");
			atp->func(tl);
			return (1);
		}
	}
	sym = VCC_FindSymbol(tl, tl->t, SYM_NONE);
	if (sym != NULL && sym->kind == SYM_PROC) {
		vcc_Expr_Call(tl, sym);
		return (1);
	}
	return (0);
}
