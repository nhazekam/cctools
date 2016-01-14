/*
 * Copyright (C) 2015- The University of Notre Dame
 * This software is distributed under the GNU General Public License.
 * See the file COPYING for details.
 * */

#include "stringtools.h"

#include "makeflow_alloc.h"

#include "list.h"
#include <string.h>
#include <stdlib.h>

struct makeflow_alloc_unit * makeflow_alloc_unit_create(int nodeid)
{
	struct makeflow_alloc_unit *u = malloc(sizeof(*u));
	u->nodeid = nodeid;
	u->total = 0;
	u->committed = 0;
	u->used = 0;

	return u;
}

struct makeflow_alloc * makeflow_alloc_create(struct makeflow_alloc *parent)
{
	struct makeflow_alloc *a = malloc(sizeof(*a));
	a->storage  = makeflow_alloc_unit_create(-1);
	a->parent = parent;
	a->residuals = list_create();

	return a;
}

int makeflow_alloc_unit_commit( struct makeflow_alloc_unit *unit, uint64_t estimated )
{
	if((unit->used + unit->committed + estimated) > unit->total)
		return 1;

	unit->committed += estimated;
	return 0;
}

int makeflow_alloc_unit_commit_free  ( struct makeflow_alloc_unit *unit, uint64_t free )
{
	unit->committed -= free;
	return 0;
}

int makeflow_alloc_unit_use ( struct makeflow_alloc_unit *unit, uint64_t estimated, uint64_t used )
{
	if(((unit->used + used) + (unit->committed - estimated)) > unit->total)
		return 1;

	unit->committed -= estimated;
	unit->used += used;
	return 0;

}

int makeflow_alloc_unit_free  ( struct makeflow_alloc_unit *unit, uint64_t free )
{
	unit->used -= free;
	return 0;
}

int makeflow_alloc_commit( struct makeflow_alloc *a, uint64_t storage_est)
{
	int storage = makeflow_alloc_unit_commit( a->storage, storage_est);

	if(core || storage || mem){
		if(!storage)
			makeflow_alloc_unit_commit_free( a->storage,  storage_est);
		return 1;
	}
	return 0;
} 

int makeflow_alloc_use( struct makeflow_alloc *a, uint64_t storage_est, uint64_t storage_use)
{
	int storage = makeflow_alloc_unit_use( a->storage, storage_est, storage_use);

	if(core || storage || mem){
		if(!storage)
			makeflow_alloc_unit_free( a->storage,  storage_est);
		return 1;
	}
	return 0;
}

int makeflow_alloc_free( struct makeflow_alloc *a, uint64_t storage_u)
{
	makeflow_alloc_unit_free( a->storage, storage_u);
	return 0;
}

int makeflow_alloc_viable_space( struct makeflow_alloc *a, struct dag_node n)
{
	int64_t space = 0;
	if(!a)
		return 0;

	struct makeflow_alloc alloc = NULL;
	list_first_item(a->residuals);
	while((alloc = list_next_item(a->residuals))){
		if(alloc->nodeid != list_peek_tail(n->res_nodes)->n->nodeid)
			continue;

		space = makeflow_alloc_check_node_size(alloc, n);
	}
	return space + alloc->free;
}

int makeflow_alloc_shrink_alloc( struct makeflow_alloc *a, int shrink_limit, int64_t dec)
{
	if(!a) // Case that we are at parent and we will now try to grow
		return 0;
	struct makeflow_alloc *tmp = a->parent;
	if(tmp->nodeid == shrink_limit) {
		a->storage->total    -= dec;
		a->storage->free     -= dec;
		return 1;
	} else if(makeflow_alloc_grow_alloc(tmp, dec)) {
		tmp->storage->commit -= dec;	
		a->storage->total    -= dec;
		a->storage->free     -= dec;
		return 1;
	} else { 
		return 0;
	}
}

int makeflow_alloc_grow_alloc( struct makeflow_alloc *a, int64_t inc)
{
	if(!a) // Case that we are at parent and we will now try to grow
		return 0;
	struct makeflow_alloc *tmp = a->parent;
	if(tmp->storage->free > inc) {
		tmp->storage->commit += inc;	
		tmp->storage->free   -= inc;	
		a->storage->total    += inc;
		a->storage->free     += inc;
		return 1;
	} else if(makeflow_alloc_grow_alloc(tmp, inc)) {
		tmp->storage->commit += inc;	
		tmp->storage->free   -= inc;	
		a->storage->total    += inc;
		a->storage->free     += inc;
		return 1;
	} else { 
		return 0;
	}
}


int makeflow_alloc_commit_space( struct makeflow_alloc *a, struct dag_node n)
{
	int64_t space = 0;
	if(!a)
		return 0;

	struct makeflow_alloc alloc = NULL;
	list_first_item(a->residuals);
	while((alloc = list_next_item(a->residuals))){
		if(alloc->nodeid != list_peek_tail(n->res_nodes)->n->nodeid)
			continue;

		space = makeflow_alloc_check_node_size(alloc, n);
	}
	return space + alloc->free;
}

int makeflow_alloc_check_node_size( struct makeflow_alloc *a, struct dag_node n)
{
	int64_t result = makeflow_alloc_viable_space(a, n);
	if(result > list_next_item(n->wgt_nodes)->n->size){
		return 0;
	}
	return 1;
}

int makeflow_alloc_commit_node_size( struct makeflow_alloc *a, struct dag_node n)
{
	int64_t result = makeflow_alloc_commit_space(a, n);
	if(result > list_next_item(n->wgt_nodes)->n->size){
		return 0;
	}
	return 1;
}
