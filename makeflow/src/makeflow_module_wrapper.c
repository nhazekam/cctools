/*
 Copyright (C) 2018- The University of Notre Dame
 This software is distributed under the GNU General Public License.
 See the file COPYING for details.
 */

#include "dag_file.h"
#include "makeflow_log.h"
#include "makeflow_hook.h"

#include "batch_task.h"
#include "batch_wrapper.h"

#include "debug.h"
#include "list.h"
#include "jx.h"
#include "jx_parse.h"
#include "stringtools.h"
#include "xxmalloc.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int makeflow_module_wrapper_register(struct makeflow_hook *h, struct list *hooks, struct jx **args)
{
	*args = jx_object(NULL);
	return MAKEFLOW_HOOK_SUCCESS;
}

struct wrapper_instance {
	char * jx;
};

struct wrapper_instance *wrapper_instance_create()
{
	struct wrapper_instance *w = calloc(1, sizeof(*w));

	return w;
}

static int makeflow_module_wrapper_create( void ** instance_struct, struct jx *hook_args )
{
	struct wrapper_instance *w = wrapper_instance_create();
	*instance_struct = w;

	struct jx *wrapper = jx_lookup (hook_args, "wrapper_jx");
	if (wrapper && jx_istype(wrapper, JX_STRING)){
		w->jx = xxstrdup(wrapper->u.string_value);
	} else {
		debug(D_MAKEFLOW_HOOK, "Must specify jx wrapper definition");
		return MAKEFLOW_HOOK_FAILURE;
	}

	return MAKEFLOW_HOOK_SUCCESS;
}

static int makeflow_module_wrapper_node_submit( void * instance_struct, struct dag_node *node, struct batch_task *task){
	struct wrapper_instance *w = (struct wrapper_instance*)instance_struct;
	assert(w);
	assert(w->jx);

	struct jx *jx_wrapper = jx_parse_file(w->jx);
	if (!jx_wrapper){
		debug(D_MAKEFLOW_HOOK, "failed to parse jx wrapper: %s", w->jx);
		return MAKEFLOW_HOOK_FAILURE;
	}

	debug(D_MAKEFLOW_HOOK, "Applying wrapper %s to task %d", w->jx, task->taskid);

	batch_task_apply_wrapper(task, jx_wrapper);

	struct batch_file *bf = NULL;
	struct dag_file *df = NULL;
	struct list_cursor *cur = list_cursor_create(task->input_files);
	for (list_seek (cur, -1); list_get (cur, (void **) &bf); list_prev (cur)){
		df = dag_file_from_name(node->d, bf->outer_name);
		if(!df){
			df = dag_file_lookup_or_create(node->d, bf->outer_name);
			df->type = DAG_FILE_TYPE_GLOBAL;
		}
		if(df->hash && bf->hash && strcmp(df->hash, bf->hash)){
			debug(D_MAKEFLOW_HOOK, "file %s already exists with different hash in task %d: %.5s %.5s", 
				bf->outer_name, task->taskid, df->hash, bf->hash);
			return MAKEFLOW_HOOK_FAILURE;
		} else if(bf->hash && !df->hash) {
			debug(D_MAKEFLOW_HOOK, "file %s setting different hash in task %d: %.5s", 
				bf->outer_name, task->taskid, bf->hash);
			df->hash = xxstrdup(bf->hash);
		} else if(!bf->hash && df->hash) {
			debug(D_MAKEFLOW_HOOK, "file %s setting different hash in task %d: %.5s", 
				bf->outer_name, task->taskid, df->hash);
			bf->hash = xxstrdup(df->hash);
		}
	}
	list_cursor_destroy(cur);

	cur = list_cursor_create(task->output_files);
	for (list_seek (cur, -1); list_get (cur, (void **) &bf); list_prev (cur)){
		df = dag_file_lookup_or_create(node->d, bf->outer_name);
		if(df->hash && ((bf->hash && strcmp(df->hash, bf->hash)) || !bf->hash)) {
			debug(D_MAKEFLOW_HOOK, "file %s already exists with different hash in task %d: %.5s %.5s", 
				bf->outer_name, task->taskid, df->hash, bf->hash);
			return MAKEFLOW_HOOK_FAILURE;
		} else if(bf->hash && !df->hash) {
			debug(D_MAKEFLOW_HOOK, "file %s setting different hash in task %d: %.5s", 
				bf->outer_name, task->taskid, bf->hash);
			df->hash = xxstrdup(bf->hash);
		}
	}
	list_cursor_destroy(cur);

	// Skip first 2 characters which are "./"
	char *base_script = string_format("%s", task->command->command+2);
	df = dag_file_lookup_or_create(node->d, base_script);
	df->type = DAG_FILE_TYPE_TEMP;
	free(base_script);

	return MAKEFLOW_HOOK_SUCCESS;
}

struct makeflow_hook makeflow_hook_wrapper = {
	.module_name = "Wrapper",
	.register_hook = makeflow_module_wrapper_register,
	.create = makeflow_module_wrapper_create,
	.node_submit = makeflow_module_wrapper_node_submit,
};


