/*
 Copyright (C) 2018- The University of Notre Dame
 This software is distributed under the GNU General Public License.
 See the file COPYING for details.
 */

#include "stringtools.h"
#include "xxmalloc.h"
#include "debug.h"
#include "batch_wrapper.h"

#include "dag.h"
#include "dag_file.h"
#include "makeflow_gc.h"
#include "makeflow_log.h"
#include "makeflow_hook.h"

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#define CONTAINER_SINGULARITY_SH "./singularity.wrapper.sh"

struct failure_handler {
	struct hash_table *failure_modes;
};

struct failure_handler *failure_handler_create()
{
	struct failure_handler *fh = malloc(sizeof(*fh));
	fh->failure_modes = hash_table_create(0, 0);

	return fh;
}

static int create( void ** instance_struct, struct jx *hook_args )
{
	struct failure_handler *fh = failure_handler_create();
	*instance_struct = fh;

	assert(fh->failure_modes);
	int rc = MAKEFLOW_HOOK_SUCCESS;

	//JX ARRAY ITERATE
	struct jx *array = jx_lookup(hook_args, "failure_conditions");
	if (array && array->type == JX_ARRAY) {
		struct jx *item = NULL;
		while((item = jx_array_shift(array))) {
			if(item->type == JX_STRING){
				char *mode = xxstrdup(item->u.string_value);

				char *handler, *p;
				p = strchr(mode, '=');
				if(p) {
					*p = 0;
					handler = xxstrdup(p+1);
					hash_table_insert(fh->failure_modes, mode, handler);
					debug(D_MAKEFLOW_HOOK, "Failure Handler handles : %s : %s ", mode, handler);
					*p = '=';
				} else {
					debug(D_ERROR|D_MAKEFLOW_HOOK, "Failure Handler expected mode=handler, got %s ", mode);
					rc = MAKEFLOW_HOOK_FAILURE;
				}
				free(mode);
			} else {
				debug(D_ERROR|D_MAKEFLOW_HOOK, "Non-string argument passed to Shared FS hook");
				rc = MAKEFLOW_HOOK_FAILURE;
			}
			jx_delete(item);
		}
	}

	return rc;
}

static int destroy( void * instance_struct, struct dag *d)
{
	struct failure_handler *fh = (struct failure_handler*)instance_struct;
	// this will leak mem, needs fix
	hash_table_delete(fh->failure_modes);
	free(fh);
	return MAKEFLOW_HOOK_SUCCESS;
}

static int node_submit( void * instance_struct, struct dag_node *n, struct batch_task *t){
	struct failure_handler *fh = (struct failure_handler*)instance_struct;
	struct batch_wrapper *wrapper = batch_wrapper_create();
	batch_wrapper_prefix(wrapper, "./failure_handler_");

	batch_wrapper_cmd(wrapper, t->command);

	char *fm;
	void *handler;
	char *repeat = NULL; 
	char *fail_state = strdup("");
	char *fail_state_tmp = NULL;
	hash_table_firstkey(fh->failure_modes);
	while(hash_table_nextkey(fh->failure_modes, &fm, &handler)){
		char *fail_state_tmp = string_format("%s%sif [ $EXIT -eq %s ]; then %s; ",
						fail_state, repeat?repeat:"", fm, (char *)handler);
		if(!repeat)
			repeat = "el";
		free(fail_state);
		fail_state = fail_state_tmp;
		makeflow_hook_add_input_file(n->d, t, handler, handler, DAG_FILE_TYPE_GLOBAL);
	}
	fail_state_tmp = string_format("%s fi", fail_state);
	free(fail_state);
	fail_state = fail_state_tmp;
	batch_wrapper_post(wrapper, fail_state);
	free(fail_state);

	char *cmd = batch_wrapper_write(wrapper, t);
	if(cmd){
		batch_task_set_command(t, cmd);
		struct dag_file *df = makeflow_hook_add_input_file(n->d, t, cmd, cmd, DAG_FILE_TYPE_TEMP);
		debug(D_MAKEFLOW_HOOK, "Wrapper written to %s", df->filename);
		makeflow_log_file_state_change(n->d, df, DAG_FILE_STATE_EXISTS);
	} else {
		debug(D_MAKEFLOW_HOOK, "Failed to create wrapper: errno %d, %s", errno, strerror(errno));
		return MAKEFLOW_HOOK_FAILURE;
	}
	free(cmd);

	return MAKEFLOW_HOOK_SUCCESS;
}
	
struct makeflow_hook makeflow_hook_failure_handler = {
	.module_name = "Failure Handler",
	.create = create,
	.destroy = destroy,

	.node_submit = node_submit,
};


