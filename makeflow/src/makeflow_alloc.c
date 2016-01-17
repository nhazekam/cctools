/*
 * Copyright (C) 2015- The University of Notre Dame
 * This software is distributed under the GNU General Public License.
 * See the file COPYING for details.
 * */

#include "stringtools.h"

#include "dag_node.h"
#include "makeflow_alloc.h"

#include "list.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

struct makeflow_alloc_unit * makeflow_alloc_unit_create(uint64_t size)
{
	struct makeflow_alloc_unit *u = malloc(sizeof(*u));
	u->total = size;
	u->commit = 0;
	u->free = size;

	return u;
}

struct makeflow_alloc * makeflow_alloc_create(int nodeid, struct makeflow_alloc *parent, uint64_t size, int locked)
{
	struct makeflow_alloc *a = malloc(sizeof(*a));
	a->nodeid = nodeid;
	a->storage  = makeflow_alloc_unit_create(size);
	a->parent = parent;
	a->residuals = list_create();
	a->locked = locked;

	return a;
}

int makeflow_alloc_viable_space( struct makeflow_alloc *a, struct dag_node *n)
{
	if(!a)
		return 0;

	struct makeflow_alloc *alloc1, *alloc2;
	struct dag_node_size *node = list_next_item(n->res_nodes);
	uint64_t space;
	if(!node)
		return a->storage->free;

	alloc1 = a;
	space = alloc1->storage->free;
	list_first_item(alloc1->residuals);
	while((alloc2 = list_next_item(alloc1->residuals)) && node){
		if(alloc2->nodeid == node->n->nodeid){
			alloc1 = alloc2;
			list_first_item(alloc1->residuals);
			node = list_next_item(n->res_nodes);
			space += alloc1->storage->free;
		}
	}
	return space;
}

int makeflow_alloc_res_accounted( struct makeflow_alloc *a, struct dag_node *n)
{
	if(!a)
		return 0;

	list_first_item(n->wgt_nodes);
	struct dag_node_size *node = list_next_item(n->wgt_nodes);
	struct makeflow_alloc *alloc1, *alloc2;

	alloc1 = a;
	printf("Residual for %d : %d : %d.\n",n->nodeid, node->n->nodeid, alloc1->nodeid);
	printf("\n\t%d", alloc1->nodeid);
	list_first_item(alloc1->residuals);
	while(node && (alloc2 = list_next_item(alloc1->residuals))){
		printf("\t%d", alloc2->nodeid);
		if(n->nodeid == alloc2->nodeid)
			break;
		if(alloc2->nodeid == node->n->nodeid){
			alloc1 = alloc2;
			printf("\n\t%d", list_size(alloc1->residuals));
			list_first_item(alloc1->residuals);
			node = list_next_item(n->wgt_nodes);
			continue;
		}
	}
	
	struct dag_node_size *tmp = list_pop_tail(n->wgt_nodes);
	node = list_peek_tail(n->wgt_nodes);
	list_push_tail(n->wgt_nodes, tmp);
	if ((alloc1->nodeid == -1 && !node) || 
		(alloc1 && node && (alloc1->nodeid == node->n->nodeid)))
			return 1;
	return 0;
}



int makeflow_alloc_check_node_size( struct makeflow_alloc *a, struct dag_node *n)
{
	if(!a->locked){
		printf("Base allocation has no limit.\n");
		return 1;
	}

	uint64_t result = makeflow_alloc_viable_space(a, n);
	struct dag_node_size *node_size = list_peek_tail(n->wgt_nodes);
	if(result >= node_size->size){
		printf("Even space in general.\n");
		return 1;
	}

	int res_accounted = makeflow_alloc_res_accounted(a, n);
	if(res_accounted && (result >= n->target_size)){
		printf("Residual is allocatd.\n");
		return 1;
	}
	return 0;
}


int makeflow_alloc_shrink_alloc( struct makeflow_alloc *a, uint64_t dec, int free)
{
	if(!a) // Case that we are at parent and we will now try to grow
		return 0;

	a->storage->total    -= dec;
	if(free)
		a->storage->commit   -= dec;
	else
		a->storage->free     -= dec;

	if(a->parent){
		a->parent->storage->commit   -= dec;
		a->parent->storage->free     += dec;
	}
	return 1;
}

int makeflow_alloc_grow_alloc( struct makeflow_alloc *a, uint64_t inc)
{
	if(!a) // Case that we are at parent and we will now try to grow
		return 0;
	struct makeflow_alloc *tmp = a->parent;
	if(a->nodeid == -1 && !a->locked){
		a->storage->total    += inc;
		a->storage->free     += inc;
		return 1;
	} else if(tmp && (tmp->storage->free >= inc || 
			makeflow_alloc_grow_alloc(tmp, (inc - tmp->storage->free)))) {
		tmp->storage->commit += inc;	
		tmp->storage->free   -= inc;	
		a->storage->total    += inc;
		a->storage->free     += inc;
		return 1;
	} else { 
		return 0;
	}
}

int makeflow_alloc_commit_space( struct makeflow_alloc *a, struct dag_node *n)
{
	if(!a)
		return 0;

	struct dag_node_size *node;
	struct makeflow_alloc *alloc1, *alloc2, *tmp;

	alloc1 = a;
	list_first_item(n->wgt_nodes);
	list_first_item(n->wgt_nodes);
	node = list_peek_tail(n->wgt_nodes);
	printf("Committing\t%" PRIu64 "\t from %d\n", node->size, n->nodeid);
	while((node = list_peek_current(n->wgt_nodes))){
		tmp = NULL;
		list_first_item(alloc1->residuals);
		while((alloc2 = list_next_item(alloc1->residuals))){
			if(alloc2->nodeid == node->n->nodeid)
				tmp = alloc2;
				break;
		}
		if(tmp)
			alloc1 = tmp;
		else
			break;
		
		list_next_item(n->wgt_nodes);
	}

	while((node = list_next_item(n->wgt_nodes))){
		alloc2 = makeflow_alloc_create(node->n->nodeid, alloc1, 0, 0);
		list_push_tail(alloc1->residuals, alloc2);
		alloc1 = alloc2;
	}
	// We have accounted for all of its residuals so only need to count for itself
	node = list_peek_tail(n->wgt_nodes);
	if(node->size > alloc1->storage->free){
		if(!makeflow_alloc_grow_alloc(alloc1, node->size - alloc1->storage->free))
			return 0;
	}
	alloc1->storage->commit += n->target_size;
	alloc1->storage->free   -= n->target_size;

	return 1;
}

void makeflow_alloc_print( struct makeflow_alloc *a, struct dag_node *n)
{
	struct dag_node_size *node;
	struct makeflow_alloc *alloc1, *alloc2, *tmp;

	printf("Alloc\t:Total\t:Commit\t:Free\n");
	alloc1 = a;
	list_first_item(n->wgt_nodes);
	while((node = list_peek_current(n->wgt_nodes))){
		printf("%d\t:%" PRIu64 "\t:%"PRIu64 "\t:%"PRIu64"\n",alloc1->nodeid, alloc1->storage->total, alloc1->storage->commit, alloc1->storage->free);
		tmp = NULL;
		list_first_item(alloc1->residuals);
		while((alloc2 = list_next_item(alloc1->residuals))){
			if(alloc2->nodeid == node->n->nodeid)
				tmp = alloc2;
				break;
		}
		if(tmp)
			alloc1 = tmp;
		else
			break;
		
		list_next_item(n->wgt_nodes);
	}
	if(tmp)
		printf("%d\t:%" PRIu64 "\t:%"PRIu64 "\t:%"PRIu64"\n",alloc1->nodeid, alloc1->storage->total, alloc1->storage->commit, alloc1->storage->free);
}

int makeflow_alloc_release_space( struct makeflow_alloc *a, struct dag_node *n, uint64_t size, int free)
{
	printf("Releasing\t%" PRIu64 "\t from %d\n", size, n->nodeid);
	struct dag_node_size *node;
	struct makeflow_alloc *alloc1, *alloc2, *tmp;

	alloc1 = a;
	list_first_item(n->wgt_nodes);
	while((node = list_next_item(n->wgt_nodes))){
		tmp = NULL;
		list_first_item(alloc1->residuals);
		while((alloc2 = list_next_item(alloc1->residuals))){
			if(alloc2->nodeid == node->n->nodeid){
				tmp = alloc2;
				break;
			}
		}
		if(tmp)
			alloc1 = tmp;
		else
			return 0;
	}
	if(alloc1->nodeid != n->nodeid)
		return 0;

	makeflow_alloc_shrink_alloc(alloc1, size, free);
	return 1;
}
