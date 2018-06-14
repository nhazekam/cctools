/*
Copyright (C) 2018- The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file COPYING for details.
*/

#include <string.h>
#include <errno.h>

#include "batch_task.h"
#include "batch_file.h"
#include "batch_wrapper.h"
#include "jx_eval.h"
#include "jx_pretty_print.h"
#include "sha1.h"
#include "rmsummary.h"
#include "stringtools.h"
#include "xxmalloc.h"
#include "debug.h"

struct batch_command *
batch_command_create ()
{
	struct batch_command *c = calloc (1, sizeof (*c));

	c->pre = list_create ();
	c->post = list_create ();

	return c;
}

void
batch_command_delete (struct batch_command *c)
{
	list_free(c->pre);
	list_delete(c->pre);
	list_free(c->post);
	list_delete(c->post);
	free (c->command);
}

/** Creates new batch_task and initializes file lists. */
struct batch_task *batch_task_create (struct batch_queue *queue)
{
	struct batch_task *t = calloc (1, sizeof (*t));

	t->queue = queue;

	t->command = batch_command_create ();

	t->input_files = list_create ();
	t->output_files = list_create ();

	t->info = batch_job_info_create ();

	return t;
}

/** Deletes task struct and frees contained data. */
void batch_task_delete (struct batch_task *t)
{
	if (!t)
	  return;

	batch_command_delete (t->command);

	struct batch_file *f;
	list_first_item (t->input_files);
	while ((f = list_next_item (t->input_files))){
	    batch_file_delete (f);
	}
	list_delete (t->input_files);

	list_first_item (t->output_files);
	while ((f = list_next_item (t->output_files)))
	  {
	    batch_file_delete (f);
	  }
	list_delete (t->output_files);

	rmsummary_delete (t->resources);

	jx_delete (t->envlist);

	batch_job_info_delete (t->info);

	free (t);
}

void batch_task_clear_command(struct batch_task *t)
{
	batch_command_delete(t->command);
	t->command = batch_command_create();
}

struct jx * batch_command_to_jx (struct batch_command *c)
{
	struct jx *cmd = jx_object (NULL);
	char *command = NULL;

	struct jx *post = jx_array (NULL);
	struct list_cursor *cur = list_cursor_create (c->post);
	for (list_seek (cur, -1); list_get (cur, (void **) &command); list_prev (cur)) {
		jx_array_insert (post, jx_string (command));
	}
	list_cursor_destroy (cur);
	jx_insert (cmd, jx_string ("post"), post);

	jx_insert (cmd, jx_string ("command"), jx_string (c->command));

	struct jx *pre = jx_array (NULL);
	cur = list_cursor_create (c->pre);
	for (list_seek (cur, -1); list_get (cur, (void **) &command); list_prev (cur)){
	    jx_array_insert (pre, jx_string (command));
	}
	list_cursor_destroy (cur);
	jx_insert (cmd, jx_string ("pre"), pre);

	return cmd;
}

/** Creates new batch_file and adds to files. */
struct batch_file *batch_task_check_file(struct list *files, struct batch_file *file)
{
	struct batch_file *f = NULL;

	struct list_cursor *cur = list_cursor_create(files);
	for (list_seek (cur, -1); list_get (cur, (void **) &f); list_prev (cur)){
		int already_exists = 0;
		if(!strcmp(f->outer_name, file->outer_name)){
			already_exists = 1;
		}
		if(file->inner_name && !strcmp(f->inner_name, file->inner_name)){
			already_exists = 1;
		}
		if(already_exists){
			if((!f->hash && !file->hash) || 
				(f->hash && file->hash && !strcmp(f->hash, file->hash))){
				list_cursor_destroy(cur);
				return file;
			} else {
				list_cursor_destroy(cur);
				batch_file_delete(file);
				return NULL;
			}
		}
	}
	list_cursor_destroy(cur);

	list_push_tail (files, file);

	return file;
}


/** Creates new batch_file and adds to inputs. */
struct batch_file *batch_task_add_input_file (struct batch_task *task, const char *outer_name, const char *inner_name, const char *hash)
{
	struct batch_file *f = batch_file_create (task->queue, outer_name, inner_name, hash);

	f = batch_task_check_file(task->input_files, f);

	if(!f)
		debug(D_BATCH, "Input %s->%s already specified in %d", outer_name, inner_name, task->taskid);

	return f;
}

/** Creates new batch_file and adds to outputs. */
struct batch_file *batch_task_add_output_file (struct batch_task *task, const char *outer_name, const char *inner_name, const char *hash)
{
	struct batch_file *f = batch_file_create (task->queue, outer_name, inner_name, hash);

	f = batch_task_check_file(task->output_files, f);

	if(!f)
		debug(D_BATCH, "Output %s->%s already specified in %d", outer_name, inner_name, task->taskid);

	return f;
}

void batch_task_add_pre (struct batch_task *t, const char *pre)
{
	assert (t->command);
	assert (t->command->pre);
	list_push_tail (t->command->pre, xxstrdup (pre));
}

/** Free previous command and strdup passed command. */
void batch_task_set_command (struct batch_task *t, const char *command)
{
	assert (t->command);
	free (t->command->command);
	t->command->command = xxstrdup (command);
}

void batch_task_add_post (struct batch_task *t, const char *post)
{
	assert (t->command);
	assert (t->command->post);
	list_push_tail (t->command->post, xxstrdup (post));
}


/** Wraps the specified command using string_wrap_command.
 See stringtools for a more detailed example of its use.
 Frees the previously set command after wrapping.
*/
void batch_task_wrap_command (struct batch_task *t, const char *command)
{
	if (!command) return;
	assert (t->command);

	char *id = string_format ("%d", t->taskid);
	char *wrap_tmp = string_replace_percents (command, id);

	free (id);

	char *result = string_wrap_command (t->command->command, wrap_tmp);
	free (wrap_tmp);

	free (t->command->command);
	t->command->command = result;
}

/** Sets the resources of batch_task.
 Uses rmsummary_copy to create a deep copy of resources.
*/
void batch_task_set_resources (struct batch_task *t, const struct rmsummary *resources)
{
	rmsummary_delete (t->resources);
	t->resources = rmsummary_copy (resources);
}

/** Sets the envlist of batch_task.
 Uses jx_copy to create a deep copy.
*/
void batch_task_set_envlist (struct batch_task *t, struct jx *envlist)
{
	jx_delete (t->envlist);
	t->envlist = jx_copy (envlist);
}

/** Sets the batch_job_info of batch_task.
 Manually copies data into struct.
 Does not free in current code, but as this become standard
 in batch_job interface we should.
*/
void batch_task_set_info (struct batch_task *t, struct batch_job_info *info)
{
	t->info->submitted = info->submitted;
	t->info->started = info->started;
	t->info->finished = info->finished;
	t->info->exited_normally = info->exited_normally;
	t->info->exit_code = info->exit_code;
	t->info->exit_signal = info->exit_signal;
	t->info->disk_allocation_exhausted = info->disk_allocation_exhausted;
}

void batch_task_set_command_spec (struct batch_task *t, struct jx *command)
{
	char *new_command = batch_wrapper_expand (t, command);
	if (!new_command){
	    int saved_errno = errno;
	    debug (D_NOTICE | D_BATCH, "failed to expand wrapper command: %s", strerror (errno));
	    errno = saved_errno;
	    return;
	}
	batch_task_set_command (t, new_command);
	free (new_command);
}

/* Return the content based ID for a node.
 * This includes :
 *  command
 *  input files (content)
 *  output files (name) : 
 *	    important addition as changed expected outputs may not 
 *	    be reflected in the command and not present in archive
 *  LATER : environment variables (name:value)
 *  returns a string the caller needs to free
 **/
char * batch_task_generate_id(struct batch_task *t) {
	if(t->hash)
		free(t->hash);
	debug(D_BATCH, "Hashing batch task %d", t->taskid);
	unsigned char *hash = xxcalloc(1, sizeof(char *)*SHA1_DIGEST_LENGTH);
	struct batch_file *f;

	sha1_context_t context;
	sha1_init(&context);

	/* Add command to the archive id */
	sha1_update (&context, "C", 1);
	sha1_update (&context, t->command->command, strlen (t->command->command));
	sha1_update (&context, "\0", 1);

	/* Sort inputs for consistent hashing */
	list_sort (t->input_files, batch_file_outer_compare);

	/* add checksum of the node's input files together */
	struct list_cursor *cur = list_cursor_create(t->input_files);
	for(list_seek(cur, 0); list_get(cur, (void**)&f); list_next(cur)) {
		char * file_id = batch_file_generate_id(f);
		sha1_update(&context, "I", 1);
		sha1_update(&context, f->outer_name, strlen(f->outer_name));
		sha1_update(&context, "C", 1);
		sha1_update(&context, file_id, strlen(file_id));
		sha1_update(&context, "\0", 1);
		free(file_id);
	}
	list_cursor_destroy (cur);

	/* Sort outputs for consistent hashing */
	list_sort (t->output_files, batch_file_outer_compare);

	/* add checksum of the node's output file names together */
	cur = list_cursor_create (t->output_files);
	for (list_seek (cur, 0); list_get (cur, (void **) &f); list_next (cur)) {
	    sha1_update (&context, "O", 1);
	    sha1_update (&context, f->outer_name, strlen (f->outer_name));
	    sha1_update (&context, "\0", 1);
	}
	list_cursor_destroy (cur);

	sha1_final(hash, &context);
	t->hash = xxstrdup(sha1_string(hash));
	free(hash);
	return xxstrdup(t->hash);
}

struct jx *batch_task_to_jx (struct batch_task *t)
{
	struct jx *tj = jx_object (NULL);

	struct batch_file *f;
	struct list_cursor *cur;

	jx_insert (tj, jx_string ("env_list"), jx_copy (t->envlist));

	jx_insert (tj, jx_string ("resources"),
	     rmsummary_to_json (t->resources, 1));

	struct jx *outputs = jx_array (NULL);
	cur = list_cursor_create (t->output_files);
	for (list_seek (cur, 0); list_get (cur, (void **) &f); list_next (cur)){
	    jx_array_insert (outputs, batch_file_to_jx (f));
	}
	list_cursor_destroy (cur);
	jx_insert (tj, jx_string ("outputs"), outputs);

	struct jx *inputs = jx_array (NULL);
	cur = list_cursor_create (t->input_files);
	for (list_seek (cur, 0); list_get (cur, (void **) &f); list_next (cur)){
	    jx_array_insert (inputs, batch_file_to_jx (f));
	}
	list_cursor_destroy (cur);
	jx_insert (tj, jx_string ("inputs"), inputs);

	jx_insert (tj, jx_string ("command"), batch_command_to_jx (t->command));
	jx_insert (tj, jx_string ("hash_id"), jx_string (batch_task_generate_id (t)));
	jx_insert (tj, jx_string ("id"), jx_string (string_format("%.8s", batch_task_generate_id (t))));
	return tj;
}

void batch_task_from_jx (struct batch_task *t, struct jx *j)
{
	assert (t);
	assert (j);

	batch_task_clear_command(t);

	struct jx *tmp;

	if (!jx_istype (j, JX_OBJECT)){
	    debug (D_NOTICE | D_BATCH, "must be a JX object");
	    return;
	}

	struct jx *inputs = jx_lookup (j, "inputs");
	if (inputs){
	    if (!jx_istype (inputs, JX_ARRAY)){
			debug (D_NOTICE | D_BATCH, "inputs must be an array");
			return;
		}

	    for (void *i = NULL; (tmp = jx_iterate_array (inputs, &i));){
			if (jx_istype(tmp, JX_STRING)){
				batch_task_add_input_file(t, xxstrdup(tmp->u.string_value), xxstrdup (tmp->u.string_value), NULL);
		    } else if (jx_istype (tmp, JX_OBJECT)) {
				char *outer_name = NULL;
				char *inner_name = NULL;
				char *hash_value = NULL;
				struct jx *outer = jx_lookup(tmp, "outer_name");
				if (outer && jx_istype(outer, JX_STRING)) {
					outer_name = xxstrdup (outer->u.string_value);
				}
				struct jx *inner = jx_lookup (tmp, "inner_name");
				if (inner && jx_istype(inner, JX_STRING)){
					inner_name = xxstrdup (inner->u.string_value);
				}
				struct jx *hash = jx_lookup (tmp, "hash");
				if (hash && jx_istype(hash, JX_STRING)){
					hash_value = xxstrdup (hash->u.string_value);
				}
				batch_task_add_input_file (t, outer_name, inner_name, hash_value);
			} else {
				debug (D_ERROR | D_MAKEFLOW_HOOK, "Non-compliant type passed as file");
				jx_pretty_print_stream(tmp, stdout);
				return;
			}
		}
	}

	struct jx *command = jx_lookup (j, "command");
	if (command){
	    struct jx *pre = jx_lookup (command, "pre");
	    if (pre){
			if (!jx_istype (pre, JX_ARRAY)){
				debug (D_NOTICE | D_BATCH, "pre commands must be specified in an array");
				return;
			}

			for (void *i = NULL; (tmp = jx_iterate_array (pre, &i));){
				if (!jx_istype (tmp, JX_STRING)){
					debug(D_NOTICE | D_BATCH, "pre commands must be strings");
					return;
				}
				batch_task_add_pre (t, tmp->u.string_value);
			}
		}

	    struct jx *post = jx_lookup (command, "post");
	    if (post){
			if (!jx_istype (post, JX_ARRAY)){
				debug (D_NOTICE | D_BATCH,"post commands must be specified in an array");
				return;
			}

			for (void *i = NULL; (tmp = jx_iterate_array (post, &i));){
				if (!jx_istype (tmp, JX_STRING)){
					debug (D_NOTICE | D_BATCH, "post commands must be strings");
					return;
				}
				batch_task_add_post (t, tmp->u.string_value);
			}
		}

	    struct jx *cmd = jx_lookup (command, "command");
	    if (cmd){
			if (jx_istype (cmd, JX_STRING)){
				batch_task_set_command (t, cmd->u.string_value);
			}
		}
	}

	struct jx *outputs = jx_lookup (j, "outputs");
	if (outputs){
	    if (!jx_istype (outputs, JX_ARRAY)){
			debug (D_NOTICE | D_BATCH, "outputs must be an array");
		}

	    for (void *i = NULL; (tmp = jx_iterate_array (outputs, &i));){
			if (tmp->type == JX_STRING){
				batch_task_add_output_file (t, xxstrdup (tmp->u.string_value), xxstrdup (tmp->u.string_value), NULL);
			} else if (tmp->type == JX_OBJECT) {
				char *outer_name = NULL;
				char *inner_name = NULL;
				char *hash_value = NULL;
				struct jx *outer = jx_lookup (tmp, "outer_name");
				if (outer && outer->type == JX_STRING){
					outer_name = xxstrdup (outer->u.string_value);
				}
				struct jx *inner = jx_lookup (tmp, "inner_name");
				if (inner && inner->type == JX_STRING){
					inner_name = xxstrdup (inner->u.string_value);
				}
				struct jx *hash = jx_lookup (tmp, "hash");
				if (hash && jx_istype(hash, JX_STRING)){
					hash_value = xxstrdup (hash->u.string_value);
				}
				batch_task_add_output_file (t, outer_name, inner_name, hash_value);
			} else {
				debug (D_ERROR | D_MAKEFLOW_HOOK, "Non-compliant type passed as file");
			}
		}
	}
}

char * batch_task_to_wrapper (struct batch_task *t)
{
	struct list_cursor *cur;

	struct batch_wrapper *wrapper = batch_wrapper_create ();
	batch_wrapper_prefix (wrapper, "t");
	batch_wrapper_id(wrapper, batch_task_generate_id(t));
	char *cmd = NULL;

	char *pre;
	cur = list_cursor_create (t->command->pre);
	for (list_seek (cur, 0); list_get (cur, (void **) &pre); list_next (cur)){
		batch_wrapper_pre (wrapper, pre);
	}
	list_cursor_destroy (cur);

	/* Execute the previous levels commmand. */
	batch_wrapper_cmd (wrapper, t->command->command);

	char *post;
	cur = list_cursor_create(t->command->post);
	for (list_seek (cur, 0); list_get (cur, (void **) &post); list_next (cur)){
		batch_wrapper_post (wrapper, post);
	}
	list_cursor_destroy(cur);

	char *wrap_name = batch_wrapper_write(wrapper, t);
	batch_task_add_input_file(t, wrap_name, wrap_name, NULL);
	batch_command_delete(t->command);
	t->command = batch_command_create();
	cmd = string_format("./%s", wrap_name);
	batch_task_set_command(t, cmd);
	free(cmd);
	return wrap_name;
}


char * batch_task_to_sandbox (struct batch_task *t)
{
	struct list_cursor *cur;

	struct batch_wrapper *wrapper = batch_wrapper_create ();
	batch_wrapper_prefix (wrapper, "t");
	batch_wrapper_id(wrapper, batch_task_generate_id(t));
	char *wrap_name = string_format ("%s_%.8s", "t", batch_task_generate_id(t));
	char *cmd = NULL;

	struct batch_file *f;
	cur = list_cursor_create (t->input_files);
	for (list_seek (cur, 0); list_get (cur, (void **) &f); list_next (cur)){
	    /* Skip if absolute path. */
	    if (f->inner_name[0] == '/') continue;

	    /* Add a cp for each file. Not linking as wq may already have done this. Not moving as it may be local. */
	    cmd = string_format ("mkdir -p $(dirname %s/%s) && cp -r %s %s/%s",
		       wrap_name, f->inner_name, f->outer_name, wrap_name, f->inner_name);
	    batch_wrapper_pre (wrapper, cmd);
	    free (cmd);
	}
	list_cursor_destroy (cur);

	/* Enter into sandbox_dir. */
	cmd = string_format ("cd %s", wrap_name);
	batch_wrapper_pre (wrapper, cmd);
	free (cmd);

	char *pre;
	cur = list_cursor_create (t->command->pre);
	for (list_seek (cur, 0); list_get (cur, (void **) &pre); list_next (cur)){
		batch_wrapper_pre (wrapper, pre);
	}
	list_cursor_destroy (cur);

	/* Execute the previous levels commmand. */
	batch_wrapper_cmd (wrapper, t->command->command);

	char *post;
	cur = list_cursor_create(t->command->post);
	for (list_seek (cur, 0); list_get (cur, (void **) &post); list_next (cur)){
		batch_wrapper_post (wrapper, post);
	}
	list_cursor_destroy(cur);


	/* Once the command is finished go back to working dir. */
	batch_wrapper_post (wrapper, "cd ..");

	cur = list_cursor_create (t->output_files);
	for (list_seek (cur, 0); list_get (cur, (void **) &f); list_next (cur)){
	    /* Skip if absolute path. */
	    if (f->inner_name[0] == '/') {
			continue;
		}

	    /* Copy out results to expected location. OR TRUE so that lack of one file does not
	       prevent other files from being sent back. */
	    cmd = string_format ("mkdir -p $(dirname %s) && cp -r %s/%s %s || true",
		       f->outer_name, wrap_name, f->inner_name, f->outer_name);
		batch_wrapper_post (wrapper, cmd);
		free(cmd);
	}
	list_cursor_destroy(cur);

	/* Enter into sandbox_dir. */
	cmd = string_format ("if [ $EXIT ] ; then rm -rf %s; fi", wrap_name);
	batch_wrapper_post (wrapper, cmd);
	free (cmd);

	free (wrap_name);
	wrap_name = batch_wrapper_write(wrapper, t);
	batch_task_add_input_file(t, wrap_name, wrap_name, NULL);
	batch_command_delete(t->command);
	t->command = batch_command_create();
	cmd = string_format("./%s", wrap_name);
	batch_task_set_command(t, cmd);
	free(cmd);
	return wrap_name;
}

void batch_task_apply_wrapper (struct batch_task *t, struct jx *w)
{
	if(t->hash){
		free(t->hash);
		t->hash = NULL;
	}

	char *task_script = NULL;
	if(w){
		task_script = batch_task_to_wrapper(t);
		struct jx *task_jx = batch_task_to_jx (t);
		jx_insert (task_jx, jx_string("script"), jx_string(task_script));

		struct jx *task = jx_eval (w, task_jx);
		batch_task_from_jx (t, task);
	} else {
		task_script = batch_task_to_sandbox(t);
	}
	free(task_script);
}


/* vim: set noexpandtab tabstop=4: */
