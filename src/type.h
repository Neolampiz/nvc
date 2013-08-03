//
//  Copyright (C) 2011-2012  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#ifndef _TYPE_H
#define _TYPE_H

#include <stdbool.h>

#include "ident.h"

typedef struct type *type_t;

typedef struct type_wr_ctx *type_wr_ctx_t;
typedef struct type_rd_ctx *type_rd_ctx_t;

struct tree_wr_ctx;
struct tree_rd_ctx;

typedef enum type_kind {
   T_UNRESOLVED,
   T_SUBTYPE,
   T_INTEGER,
   T_REAL,
   T_ENUM,
   T_PHYSICAL,
   T_CARRAY,
   T_UARRAY,
   T_RECORD,
   T_FILE,
   T_ACCESS,
   T_FUNC,
   T_INCOMPLETE,
   T_PROC,

   T_LAST_TYPE_KIND
} type_kind_t;

typedef enum {
   RANGE_TO,
   RANGE_DOWNTO,
   RANGE_EXPR,
   RANGE_DYN,
   RANGE_RDYN
} range_kind_t;

typedef struct range {
   struct tree  *left;
   struct tree  *right;
   range_kind_t  kind;
} range_t;

type_t type_new(type_kind_t kind);

type_kind_t type_kind(type_t t);
const char *type_kind_str(type_kind_t t);

bool type_eq(type_t a, type_t b);

// See `has_map' in type.c for definition of which fields each type
// kind contains

ident_t type_ident(type_t t);
void type_set_ident(type_t t, ident_t id);

type_t type_base(type_t t);
void type_set_base(type_t t, type_t b);

type_t type_elem(type_t t);
void type_set_elem(type_t t, type_t e);

unsigned type_dims(type_t t);
range_t type_dim(type_t t, unsigned n);
void type_add_dim(type_t t, range_t r);
void type_change_dim(type_t t, unsigned n, range_t r);

unsigned type_enum_literals(type_t t);
struct tree *type_enum_literal(type_t t, unsigned n);
void type_enum_add_literal(type_t t, struct tree *lit);

unsigned type_units(type_t t);
struct tree *type_unit(type_t t, unsigned n);
void type_add_unit(type_t t, struct tree *u);

unsigned type_params(type_t t);
type_t type_param(type_t t, unsigned n);
void type_add_param(type_t t, type_t p);

type_t type_result(type_t t);
void type_set_result(type_t t, type_t r);

type_t type_access(type_t t);
void type_set_access(type_t t, type_t a);

type_t type_file(type_t t);
void type_set_file(type_t t, type_t f);

unsigned type_index_constrs(type_t t);
void type_add_index_constr(type_t t, type_t c);
void type_change_index_constr(type_t t, unsigned n, type_t c);
type_t type_index_constr(type_t t, unsigned n);

unsigned type_fields(type_t t);
struct tree *type_field(type_t t, unsigned n);
void type_add_field(type_t t, struct tree *e);

void type_replace(type_t t, type_t a);

void type_set_resolution(type_t t, struct tree *r);
bool type_has_resolution(type_t t);
struct tree *type_resolution(type_t t);

type_wr_ctx_t type_write_begin(struct tree_wr_ctx *tree_ctx,
                               ident_wr_ctx_t ident_ctx);
void type_write(type_t t, type_wr_ctx_t ctx);
void type_write_end(type_wr_ctx_t ctx);

type_rd_ctx_t type_read_begin(struct tree_rd_ctx *tree_ctx,
                              ident_rd_ctx_t ident_ctx);
type_t type_read(type_rd_ctx_t ctx);
void type_read_end(type_rd_ctx_t ctx);

// Pretty printing
typedef const char *(*minify_fn_t)(const char *);
const char *type_pp(type_t t);
const char *type_pp_minify(type_t t, minify_fn_t fn);

// Predefined types
type_t type_universal_int(void);
type_t type_universal_real(void);
bool type_is_universal(type_t t);

// Garbage collection
void type_sweep(unsigned generation);

// Type or its parent type is an array
bool type_is_array(type_t t);

// Helper to find ultimate base type
type_t type_base_recur(type_t t);

// Helper function to find number of sub-elemets
unsigned type_width(type_t type);

uint32_t type_format_digest(void);

#endif  // _TYPE_H
