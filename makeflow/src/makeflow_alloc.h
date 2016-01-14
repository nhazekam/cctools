
/*
Copyright (C) 2015- The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file COPYING for details.
*/

#ifndef MAKEFLOW_ALLOC_H
#define MAKEFLOW_ALLOC_H

/*
This module implements resource allocations.
*/

struct makeflow_alloc_unit {
	uint64_t total;
	uint64_t committed;
	uint64_t used;
};

struct makeflow_alloc {
	struct makeflow_alloc_unit *cores;
	struct makeflow_alloc_unit *disk;
	struct makeflow_alloc_unit *memory;
};

struct makeflow_alloc * makeflow_alloc_create();
struct makeflow_alloc_unit * makeflow_alloc_unit_create();

int makeflow_alloc_commit	( struct makeflow_alloc *a, uint64_t core_est, 
								uint64_t disk_est, uint64_t memory_est);
int makeflow_alloc_use		( struct makeflow_alloc *a, uint64_t core_est, 
								uint64_t core_use, uint64_t disk_est, 
								uint64_t disk_use, uint64_t memory_est, 
								uint64_t memory_use);
int makeflow_alloc_free		( struct makeflow_alloc *a, uint64_t core_u, 
								uint64_t disk_u, uint64_t memory_u);

int makeflow_alloc_unit_commit( struct makeflow_alloc_unit *unit, uint64_t estimated );
int makeflow_alloc_unit_use   ( struct makeflow_alloc_unit *unit, uint64_t estimated, uint64_t used );
int makeflow_alloc_unit_free  ( struct makeflow_alloc_unit *unit, uint64_t free );

#endif
