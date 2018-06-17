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
#include "stringtools.h"
#include "xxmalloc.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int makeflow_module_sandbox_register(struct makeflow_hook *h, struct list *hooks, struct jx **args)
{
	*args = jx_object(NULL);
	return MAKEFLOW_HOOK_SUCCESS;
}

static int makeflow_module_sandbox_node_submit( void * instance_struct, struct dag_node *node, struct batch_task *task){

	batch_task_apply_wrapper(task, NULL);


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

	cur = list_cursor_create(task->temp_files);
	for (list_seek (cur, -1); list_get (cur, (void **) &bf); list_prev (cur)){
		df = dag_file_lookup_or_create(node->d, bf->outer_name);
		df->type = DAG_FILE_TYPE_TEMP;
	}
	list_cursor_destroy(cur);


	return MAKEFLOW_HOOK_SUCCESS;
}

struct makeflow_hook makeflow_hook_sandbox = {
	.module_name = "Sandbox",
	.register_hook = makeflow_module_sandbox_register,
	.node_submit = makeflow_module_sandbox_node_submit,
};


