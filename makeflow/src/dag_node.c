/*
Copyright (C) 2014- The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file COPYING for details.
*/

#include "dag.h"
#include "dag_node.h"

#include "debug.h"
#include "rmsummary.h"
#include "list.h"
#include "stringtools.h"
#include "xxmalloc.h"
#include "jx.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int dag_node_comp(void *item, const void *arg)
{
	struct dag_node *d = ((struct dag_node *) item);
	struct dag_node *e = ((struct dag_node *) arg);

	if(d->nodeid == e->nodeid)
		return 1;
	return 0;
}

struct dag_node *dag_node_create(struct dag *d, int linenum)
{
	struct dag_node *n;

	n = malloc(sizeof(struct dag_node));
	memset(n, 0, sizeof(struct dag_node));
	n->d = d;
	n->linenum = linenum;
	n->state = DAG_NODE_STATE_WAITING;
	n->nodeid = d->nodeid_counter++;
	n->variables = hash_table_create(0, 0);

	n->source_files = list_create();
	n->target_files = list_create();

	n->remote_names = itable_create(0);
	n->remote_names_inv = hash_table_create(0, 0);

	n->descendants = set_create(0);
	n->ancestors = set_create(0);

	n->source_size = -1;
	n->target_size = -1;

	n->res_nodes = list_create();
	n->wgt_nodes = list_create();
	n->run_nodes = list_create();
	n->updated = 0;
	n->active = 0;

	n->ancestor_depth = -1;

	n->resources = make_rmsummary(-1);

	return n;
}

struct dag_node_size *dag_node_size_create(struct dag_node *n, uint64_t size)
{
	struct dag_node_size *s;

	s = malloc(sizeof(struct dag_node_size));
	s->n = n;
	s->size = size;

	return s;
}

const char *dag_node_state_name(dag_node_state_t state)
{
	switch (state) {
	case DAG_NODE_STATE_WAITING:
		return "waiting";
	case DAG_NODE_STATE_RUNNING:
		return "running";
	case DAG_NODE_STATE_COMPLETE:
		return "complete";
	case DAG_NODE_STATE_FAILED:
		return "failed";
	case DAG_NODE_STATE_ABORTED:
		return "aborted";
	default:
		return "unknown";
	}
}

/* Returns the remotename used in rule n for local name filename */
const char *dag_node_get_remote_name(struct dag_node *n, const char *filename)
{
	struct dag_file *f;
	char *name;

	f = dag_file_from_name(n->d, filename);
	name = (char *) itable_lookup(n->remote_names, (uintptr_t) f);

	return name;
}

/* Returns the local name of filename */
const char *dag_node_get_local_name(struct dag_node *n, const char *filename)
{
	struct dag_file *f;
	const char *name;

	f = hash_table_lookup(n->remote_names_inv, filename);

	if(!f)
	{
		name =  NULL;
	}
	else
	{
		name = f->filename;
	}

	return name;
}

/* Translate an absolute filename into a unique slash-less name to allow for the
   sending of any file to remote systems. The function allows for upto a million name collisions. */
static char *dag_node_translate_filename(struct dag_node *n, const char *filename)
{
	int len;
	char *newname_ptr;

	len = strlen(filename);

	/* If there are no slashes in path, then we don't need to translate. */
	if(!strchr(filename, '/')) {
		newname_ptr = xxstrdup(filename);
		return newname_ptr;
	}

	/* If the filename is in the current directory and doesn't contain any
	 * additional slashes, then we can also skip translation.
	 *
	 * Note: this doesn't handle redundant ./'s such as ./././././foo/bar */
	if(!strncmp(filename, "./", 2) && !strchr(filename + 2, '/')) {
		newname_ptr = xxstrdup(filename);
		return newname_ptr;
	}

	/* Make space for the new filename + a hyphen + a number to
	 * handle upto a million name collisions */
	newname_ptr = calloc(len + 8, sizeof(char));
	strcpy(newname_ptr, filename);

	char *c;
	for(c = newname_ptr; *c; ++c) {
		switch (*c) {
		case '/':
		case '.':
			*c = '_';
			break;
		default:
			break;
		}
	}

	if(!n)
		return newname_ptr;

	int i = 0;
	char *newname_org = xxstrdup(newname_ptr);
	while(hash_table_lookup(n->remote_names_inv, newname_ptr)) {
		sprintf(newname_ptr, "%06d-%s", i, newname_org);
		i++;
	}

	free(newname_org);

	return newname_ptr;
}

/* Adds remotename to the local name filename in the namespace of
 * the given node. If remotename is NULL, then a new name is
 * found using dag_node_translate_filename. If the remotename
 * given is different from a previosly specified, a warning is
 * written to the debug output, but otherwise this is ignored. */
static const char *dag_node_add_remote_name(struct dag_node *n, const char *filename, const char *remotename)
{
	char *oldname;
	struct dag_file *f = dag_file_from_name(n->d, filename);

	if(!f)
		fatal("trying to add remote name %s to unknown file %s.\n", remotename, filename);

	if(!remotename)
		remotename = dag_node_translate_filename(n, filename);
	else
		remotename = xxstrdup(remotename);

	oldname = hash_table_lookup(n->remote_names_inv, remotename);

	if(oldname && strcmp(oldname, filename) == 0)
		debug(D_MAKEFLOW_RUN, "Remote name %s for %s already in use for %s\n", remotename, filename, oldname);

	itable_insert(n->remote_names, (uintptr_t) f, remotename);
	hash_table_insert(n->remote_names_inv, remotename, (void *) f);

	return remotename;
}

/* Adds the local name to the list of source files of the node,
 * and adds the node as a dependant of the file. If remotename is
 * not NULL, it is added to the namespace of the node. */
void dag_node_add_source_file(struct dag_node *n, const char *filename, char *remotename)
{
	struct dag_file *source = dag_file_lookup_or_create(n->d, filename);

	if(remotename)
		dag_node_add_remote_name(n, filename, remotename);

	/* register this file as a source of the node */
	list_push_head(n->source_files, source);

	/* register this file as a requirement of the node */
	list_push_head(source->needed_by, n);

	source->ref_count++;
}

/* Adds the local name as a target of the node, and register the
 * node as the producer of the file. If remotename is not NULL,
 * it is added to the namespace of the node. */
void dag_node_add_target_file(struct dag_node *n, const char *filename, char *remotename)
{
	struct dag_file *target = dag_file_lookup_or_create(n->d, filename);

	if(target->created_by && target->created_by != n)
		fatal("%s is defined multiple times at %s:%d and %s:%d\n", filename, filename, target->created_by->linenum, filename, n->linenum);

	if(remotename)
		dag_node_add_remote_name(n, filename, remotename);

	/* register this file as a target of the node */
	list_push_head(n->target_files, target);

	/* register this node as the creator of the file */
	target->created_by = n;
}

void dag_node_prepare_node_size(struct dag_node *n)
{
	struct dag_file *f;
	struct dag_node *s;
	n->source_size = 0;
	n->target_size = 0;
	list_first_item(n->source_files);
	while((f = list_next_item(n->source_files))){
		if(dag_file_should_exist(f))
			n->source_size += f->file_size;
		else 
			n->source_size += f->est_size;
	}
	list_first_item(n->target_files);
	while((f = list_next_item(n->target_files))){
		if(dag_file_should_exist(f))
			n->target_size += f->file_size;
		else 
			n->target_size += f->est_size;
	}

	set_first_element(n->descendants);
	while((s = set_next_element(n->descendants))){
		dag_node_prepare_node_size(s);
	}
}

void dag_node_determine_footprint(struct dag_node *n)
{
	n->parent_wgt = n->target_size;
	n->children_wgt = n->target_size;
	n->descendant_wgt = 0;

	struct dag_file *f;
	struct dag_node *d, *e;
	struct dag_node_size *s, *t;
	struct set *tmp = set_create(0);

	set_first_element(n->ancestors);
	while((d = set_next_element(n->ancestors))){
		n->parent_wgt = n->parent_wgt + d->target_size;
	}


	set_first_element(n->descendants);
	while((d = set_next_element(n->descendants))){
		n->children_wgt = n->children_wgt + d->target_size;
		while((f = list_next_item(n->source_files))){
			if(f->created_by->nodeid == n->nodeid)
				continue;
			if(dag_file_should_exist(f))
				n->children_wgt = n->children_wgt + f->file_size;
			else 
				n->children_wgt = n->children_wgt + f->est_size;
		}
		set_push(tmp, d);
	}


	set_first_element(tmp);
	while((d = set_next_element(tmp))){
		if(!d->updated)
			dag_node_determine_footprint(d);
		list_first_item(d->res_nodes);
		list_first_item(d->wgt_nodes);
	}


	list_delete(n->res_nodes);
	list_delete(n->wgt_nodes);
	list_delete(n->run_nodes);


	set_first_element(n->descendants);
	if(set_size(n->descendants) > 1){
		n->res_nodes = list_create();
		n->wgt_nodes = list_create();
		n->run_nodes = list_create();
		set_first_element(n->descendants);

		printf("Multiple Desc\n");

		int comp = 1;
		while(comp){
			d = set_next_element(n->descendants);
			s = list_next_item(d->res_nodes);
			while((d = set_next_element(n->descendants))){
				t = list_next_item(d->res_nodes);
				if((t && !s) || (s && !t) || (t->n->nodeid != s->n->nodeid))
					comp = 0;
			}
			
			set_first_element(n->descendants);
			if(comp)
				list_push_tail(n->res_nodes, s);
		}

		comp = 1;
		while(comp){
			d = set_next_element(n->descendants);
			s = list_next_item(d->wgt_nodes);
			while((d = set_next_element(n->descendants))){
				t = list_next_item(d->wgt_nodes);
				if((t && !s) || (s && !t) || (t->n->nodeid != s->n->nodeid))
					comp = 0;
			}
			
			set_first_element(n->descendants);
			if(comp)
				list_push_tail(n->wgt_nodes, s);
		}

		uint64_t node_wgt, max_wgt, tmp_wgt, res_size;
		max_wgt = 0;
		set_first_element(tmp);
		while((d = set_next_element(tmp))){
			struct list *tmp_run = list_create();
			node_wgt = d->parent_wgt;
			if(d->parent_wgt <= d->children_wgt)
				node_wgt = d->children_wgt;
			list_push_head(tmp_run, d);	

			while((t = list_peek_current(d->wgt_nodes))){
				if(t->size > node_wgt)
					node_wgt = t->size;
				list_next_item(d->wgt_nodes);
			}
			tmp_wgt = node_wgt;
			set_first_element(n->descendants);
			while((e = set_next_element(n->descendants))){
				t = list_peek_current(e->res_nodes);
				if(!t)
					res_size = e->target_size;
				else
					res_size = t->size;
				if(e->nodeid != d->nodeid){
					node_wgt += res_size;
					list_push_head(tmp_run, e);	
				}
			}

			if(max_wgt < tmp_wgt || (max_wgt == tmp_wgt && node_wgt < n->descendant_wgt)){
				max_wgt = tmp_wgt;
				n->descendant_wgt = node_wgt;
				list_delete(n->run_nodes);
				n->run_nodes = list_duplicate(tmp_run);
			} 
			list_delete(tmp_run);	
		}
	} else if(set_size(n->descendants) == 1){
		d = set_next_element(n->descendants);
		n->run_nodes = list_create();
		list_push_tail(n->run_nodes, d);

//		s = list_peek_tail(d->wgt_nodes);
//		n->descendant_wgt = s->size;
		n->res_nodes = list_duplicate(d->res_nodes);
		n->wgt_nodes = list_duplicate(d->wgt_nodes);
	} else {
		n->res_nodes = list_create();
		n->wgt_nodes = list_create();
		n->run_nodes = list_create();
	}

	list_push_tail(n->res_nodes, dag_node_size_create(n, n->target_size));

	if((n->parent_wgt >= n->children_wgt) && (n->parent_wgt >= n->descendant_wgt)){
		list_push_tail(n->wgt_nodes, dag_node_size_create(n, n->parent_wgt));	
	} else if((n->children_wgt >= n->parent_wgt) && (n->children_wgt >= n->descendant_wgt)){
		list_push_tail(n->wgt_nodes, dag_node_size_create(n, n->children_wgt));	
	} else {
		list_push_tail(n->wgt_nodes, dag_node_size_create(n, n->descendant_wgt));	
	}

/*		USED FOR CHECKING EXPECTED SIZES
*/
	printf("Parent weight for %d : %" PRIu64"\n", n->nodeid, n->parent_wgt);
	printf("Child weight for %d : %" PRIu64"\n", n->nodeid, n->children_wgt);
	printf("Desc weight for %d : %" PRIu64"\n", n->nodeid, n->descendant_wgt);

	list_first_item(n->run_nodes);
	while((d = list_next_item(n->run_nodes)))
		printf("%d\t", d->nodeid);
	printf("\n");

	list_first_item(n->wgt_nodes);
	while((s = list_next_item(n->wgt_nodes))){
		printf("(%d, %"PRIu64") ", s->n->nodeid, s->size);
	}
	printf("\n");

	list_first_item(n->res_nodes);
	while((s = list_next_item(n->res_nodes))){
		printf("(%d, %"PRIu64") ", s->n->nodeid, s->size);
	}
	printf("\n");fflush(stdout);

	n->updated = 1;
}

void dag_node_fill_resources(struct dag_node *n)
{
	struct rmsummary *rs    = n->resources;
	struct dag_variable_lookup_set s = { n->d, n->category, n, NULL };

	char    *val_str;

	val_str = dag_variable_lookup_string(RESOURCES_CORES, &s);
	if(val_str)
		rs->cores = atoll(val_str);

	val_str = dag_variable_lookup_string(RESOURCES_DISK, &s);
	if(val_str)
		rs->workdir_footprint = atoll(val_str);

	val_str = dag_variable_lookup_string(RESOURCES_MEMORY, &s);
	if(val_str)
		rs->resident_memory = atoll(val_str);

	val_str = dag_variable_lookup_string(RESOURCES_GPUS, &s);
	if(val_str)
		rs->gpus = atoll(val_str);
}

void dag_node_print_debug_resources(struct dag_node *n)
{
	if( n->resources->cores > -1 )
		debug(D_MAKEFLOW_RUN, "cores:  %"PRId64".\n",      n->resources->cores);
	if( n->resources->resident_memory > -1 )
		debug(D_MAKEFLOW_RUN, "memory:   %"PRId64" MB.\n", n->resources->resident_memory);
	if( n->resources->workdir_footprint > -1 )
		debug(D_MAKEFLOW_RUN, "disk:     %"PRId64" MB.\n", n->resources->workdir_footprint);
	if( n->resources->gpus > -1 )
		debug(D_MAKEFLOW_RUN, "gpus:  %"PRId64".\n", n->resources->gpus);
}

char *dag_node_resources_wrap_as_wq_options(struct dag_node *n, const char *default_options)
{
	struct rmsummary *s;

	s = n->resources;

	char *options = NULL;

	options = string_format("%s resources: cores: %" PRId64 ", resident_memory: %" PRId64 ", workdir_footprint: %" PRId64,
			default_options           ? default_options      : "",
			s->cores             > -1 ? s->cores             : -1,
			s->resident_memory   > -1 ? s->resident_memory   : -1,
			s->workdir_footprint > -1 ? s->workdir_footprint : -1);

	return options;
}

#define add_monitor_field_int(options, s, field) \
	if(s->field > -1) \
	{ \
		char *opt = string_format("%s -L'%s: %" PRId64 "' ", options ? options : "", #field, s->field);\
		if(options)\
			free(options);\
		options = opt;\
	}

#define add_monitor_field_double(options, s, field) \
	if(s->field > -1) \
	{ \
		char *opt = string_format("%s -L'%s: %lf' ", options ? options : "", #field, s->field/1000000.0);\
		if(options)\
			free(options);\
		options = opt;\
	}

char *dag_node_resources_wrap_as_rmonitor_options(struct dag_node *n)
{
	struct rmsummary *s;

	s = n->resources;

	char *options = NULL;

	add_monitor_field_double(options, s, wall_time);
	add_monitor_field_int(options, s, max_concurrent_processes);
	add_monitor_field_int(options, s, total_processes);
	add_monitor_field_double(options, s, cpu_time);
	add_monitor_field_int(options, s, virtual_memory);
	add_monitor_field_int(options, s, resident_memory);
	add_monitor_field_int(options, s, swap_memory);
	add_monitor_field_int(options, s, bytes_read);
	add_monitor_field_int(options, s, bytes_written);
	add_monitor_field_int(options, s, workdir_num_files);
	add_monitor_field_int(options, s, workdir_footprint);

	return options;
}

/* works as realloc for the first argument */
char *dag_node_resources_add_condor_option(char *options, const char *expression, int64_t value)
{
	if(value < 0)
		return options;

	char *opt = NULL;
	if(options)
	{
		opt = string_format("%s && (%s%" PRId64 ")", options, expression, value);
		free(options);
		options = opt;
	}
	else
	{
		options = string_format("(%s%" PRId64 ")", expression, value);
	}

	return options;
}

char *dag_node_resources_wrap_as_condor_options(struct dag_node *n, const char *default_options)
{
	struct rmsummary *s;

	s = n->resources;

	char *options = NULL;
	char *opt;

	options = dag_node_resources_add_condor_option(options, "Cores>=", s->cores);
	options = dag_node_resources_add_condor_option(options, "Memory>=", s->resident_memory);
	options = dag_node_resources_add_condor_option(options, "Disk>=", s->workdir_footprint);

	if(!options)
	{
		if(default_options)
			return xxstrdup(default_options);
		return
			NULL;
	}

	if(!default_options)
	{
		opt = string_format("Requirements = %s\n", options);
		free(options);
		return opt;
	}

	/* else default_options && options */
	char *scratch = xxstrdup(default_options);
	char *req_pos = strstr(scratch, "Requirements");
	if(!req_pos)
		req_pos = strstr(scratch, "requirements");

	if(!req_pos)
	{
		opt = string_format("Requirements = %s\n%s", options, scratch);
		free(options);
		free(scratch);
		return opt;
	}

	/* else, requirements have been specified also at default_options*/
	char *equal_sign = strchr(req_pos, '=');
	if(!equal_sign)
	{
	/* Possibly malformed, not much we can do. */
		free(options);
		free(scratch);
		return xxstrdup(default_options);
	}

	char *newline = strchr(scratch, '\n');
	if(!newline)
		newline = (scratch + strlen(default_options) - 1); /* end of string */

	*req_pos    = '\0';
	*equal_sign = '\0';
	*newline    = '\0';

	/* Now we have these different strings:
	   scratch: from beginning of default_options to the 'R' in Requirements
	   equal_sign: from the '=' after Requirements to a new line or end of default_options
	   newline: from the end of original requirements to end of default_options
	*/

	opt = string_format("%s\nRequirements = (%s) && (%s)\n%s", scratch, (equal_sign + 1), options, (newline + 1));
	free(scratch);
	free(options);

	return opt;
}

char *dag_node_resources_wrap_options(struct dag_node *n, const char *default_options, batch_queue_type_t batch_type)
{
	switch(batch_type)
	{
		case BATCH_QUEUE_TYPE_WORK_QUEUE:
			return dag_node_resources_wrap_as_wq_options(n, default_options);
			break;
		case BATCH_QUEUE_TYPE_CONDOR:
			return dag_node_resources_wrap_as_condor_options(n, default_options);
			break;
		default:
			if(default_options)
				return xxstrdup(default_options);
			else
				return NULL;
	}
}

/*
Creates a jx object containing the explicit environment
strings for this given node.
*/

struct jx * dag_node_env_create( struct dag *d, struct dag_node *n )
{
	struct dag_variable_lookup_set s = { d, n->category, n, NULL };
	char *key;

	struct jx *object = jx_object(0);

	set_first_element(d->export_vars);
	while((key = set_next_element(d->export_vars))) {
		char *value = dag_variable_lookup_string(key, &s);
		if(value) {
			jx_insert(object,jx_string(key),jx_string(value));
			debug(D_MAKEFLOW_RUN, "export %s=%s", key, value);
		}
	}

	return object;
}

/* vim: set noexpandtab tabstop=4: */
