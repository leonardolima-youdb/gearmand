/*  vim:expandtab:shiftwidth=2:tabstop=2:smarttab:
 * 
 *  Gearmand client and server library.
 *
 *  Copyright (C) 2011 Data Differential, http://datadifferential.com/
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are
 *  met:
 *
 *      * Redistributions of source code must retain the above copyright
 *  notice, this list of conditions and the following disclaimer.
 *
 *      * Redistributions in binary form must reproduce the above
 *  copyright notice, this list of conditions and the following disclaimer
 *  in the documentation and/or other materials provided with the
 *  distribution.
 *
 *      * The names of its contributors may not be used to endorse or
 *  promote products derived from this software without specific prior
 *  written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <libtest/test.h>
#include <cassert>
#include <libgearman/gearman.h>
#include <tests/task.h>

#include <iostream>

#ifndef __INTEL_COMPILER
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif

test_return_t gearman_client_add_task_test(void *object)
{
  gearman_client_st *client= (gearman_client_st *)object;
  const char *worker_function= (const char *)gearman_client_context(client);

  assert(worker_function);

  gearman_return_t ret;
  gearman_task_st *task= gearman_client_add_task(client, NULL, NULL,
                                                 worker_function, NULL, "dog", 3,
                                                 &ret);
  test_true_got(gearman_success(ret), gearman_strerror(ret));
  test_truth(task);

  do
  {
    ret= gearman_client_run_tasks(client);
    test_true_got(ret == GEARMAN_SUCCESS, gearman_client_error(client));
  } while (gearman_task_is_running(task));

  gearman_task_free(task);

  return TEST_SUCCESS;
}

test_return_t gearman_client_add_task_test_fail(void *object)
{
  gearman_client_st *client= (gearman_client_st *)object;
  const char *worker_function= (const char *)gearman_client_context(client);

  assert(worker_function);

  gearman_return_t ret;
  gearman_task_st *task= gearman_client_add_task(client, NULL, NULL,
                                                 worker_function, NULL,
                                                 gearman_literal_param("fail"),
                                                 &ret);
  test_true_got(ret == GEARMAN_SUCCESS, gearman_strerror(ret));
  test_truth(task);
  test_truth(task->client);
  do
  {
    ret= gearman_client_run_tasks(client);
    test_true_got(ret == GEARMAN_SUCCESS, gearman_client_error(client));
    test_truth(task->client);
  } while (gearman_task_is_running(task));

  test_true_got(ret == GEARMAN_SUCCESS, gearman_client_error(client));
  test_true_got(gearman_task_error(task) == GEARMAN_WORK_FAIL, gearman_strerror(gearman_task_error(task)));

  test_truth(task->client);
  gearman_task_free(task);

  return TEST_SUCCESS;
}

test_return_t gearman_client_add_task_test_bad_workload(void *object)
{
  gearman_client_st *client= (gearman_client_st *)object;
  const char *worker_function= (const char *)gearman_client_context(client);

  assert(worker_function);

  gearman_return_t ret;

  // We test for pointer with zero size
  gearman_task_st *task= gearman_client_add_task(client, NULL, NULL,
                                                 worker_function, NULL, "fail", 0,
                                                 &ret);
  test_true_got(ret == GEARMAN_INVALID_ARGUMENT, gearman_strerror(ret));
  test_false(task);

  // We test for NULL with size
  task= gearman_client_add_task(client, NULL, NULL,
                                worker_function, NULL, NULL, 5,
                                &ret);
  test_true_got(ret == GEARMAN_INVALID_ARGUMENT, gearman_strerror(ret));
  test_false(task);

  return TEST_SUCCESS;
}

static gearman_return_t gearman_exception_test_function(gearman_task_st *task)
{
  bool *success= (bool *)gearman_task_context(task);
  if (not success)
    return GEARMAN_WORK_FAIL;

  *success= true;
  return GEARMAN_SUCCESS;
}

test_return_t gearman_client_add_task_exception(void *object)
{
  gearman_client_st *client= (gearman_client_st *)object;
  const char *worker_function= (const char *)gearman_client_context(client);

  assert(worker_function);

  gearman_return_t ret;

  test_truth(gearman_client_set_server_option(client, gearman_literal_param("exceptions")));

  gearman_client_set_exception_fn(client, gearman_exception_test_function);

  bool exception_success= false;
  gearman_task_st *task= gearman_client_add_task(client, NULL, &exception_success,
                                                 worker_function, NULL,
                                                 gearman_literal_param("exception"),
                                                 &ret);
  test_true_got(ret == GEARMAN_SUCCESS, gearman_strerror(ret));
  test_truth(task);

  ret= gearman_client_run_tasks(client);
  test_true_got(ret == GEARMAN_SUCCESS, gearman_strerror(ret));
  test_truth(exception_success);

  gearman_client_set_exception_fn(client, NULL);
  gearman_task_free(task);

  return TEST_SUCCESS;
}

test_return_t gearman_client_add_task_background_test(void *object)
{
  gearman_client_st *client= (gearman_client_st *)object;
  const char *worker_function= (const char *)gearman_client_context(client);

  assert(worker_function);

  gearman_return_t ret;
  gearman_task_st *task= gearman_client_add_task_background(client, NULL, NULL,
                                                            worker_function, NULL, "dog", 3,
                                                            &ret);
  test_true_got(ret == GEARMAN_SUCCESS, gearman_strerror(ret));
  test_truth(task);

  do
  {
    ret= gearman_client_run_tasks(client);
    test_true_got(ret == GEARMAN_SUCCESS, gearman_client_error(client));
  } while (gearman_task_is_running(task));

  gearman_task_free(task);

  return TEST_SUCCESS;
}

test_return_t gearman_client_add_task_high_background_test(void *object)
{
  gearman_client_st *client= (gearman_client_st *)object;
  const char *worker_function= (const char *)gearman_client_context(client);

  assert(worker_function);

  gearman_return_t ret;
  gearman_task_st *task= gearman_client_add_task_high_background(client, NULL, NULL,
                                                                 worker_function, NULL, "dog", 3,
                                                                 &ret);
  test_true_got(ret == GEARMAN_SUCCESS, gearman_strerror(ret));
  test_truth(task);

  do
  {
    ret= gearman_client_run_tasks(client);
    test_true_got(ret == GEARMAN_SUCCESS, gearman_client_error(client));
  } while (gearman_task_is_running(task));

  gearman_task_free(task);

  return TEST_SUCCESS;
}

test_return_t gearman_client_add_task_low_background_test(void *object)
{
  gearman_client_st *client= (gearman_client_st *)object;
  const char *worker_function= (const char *)gearman_client_context(client);

  assert(worker_function);

  gearman_return_t ret;
  gearman_task_st *task= gearman_client_add_task_high_background(client, NULL, NULL,
                                                                 worker_function, NULL, "dog", 3,
                                                                 &ret);
  test_true_got(ret == GEARMAN_SUCCESS, gearman_strerror(ret));
  test_truth(task);

  do
  {
    ret= gearman_client_run_tasks(client);
    test_true_got(ret == GEARMAN_SUCCESS, gearman_client_error(client));
  } while (gearman_task_is_running(task));

  gearman_task_free(task);

  return TEST_SUCCESS;
}

static gearman_return_t gearman_warning_test_function(gearman_task_st *task)
{
  bool *success= (bool *)gearman_task_context(task);
  if (not success)
    return GEARMAN_WORK_FAIL;

  *success= true;
  return GEARMAN_SUCCESS;
}

test_return_t gearman_client_add_task_warning(void *object)
{
  gearman_client_st *client= (gearman_client_st *)object;
  const char *worker_function= (const char *)gearman_client_context(client);

  assert(worker_function);

  gearman_return_t ret;

  gearman_client_set_warning_fn(client, gearman_warning_test_function);

  bool warning_success= false;
  gearman_task_st *task= gearman_client_add_task(client, NULL, &warning_success,
                                                 worker_function, NULL,
                                                 gearman_literal_param("warning"),
                                                 &ret);
  test_true_got(ret == GEARMAN_SUCCESS, gearman_strerror(ret));
  test_truth(task);

  ret= gearman_client_run_tasks(client);
  test_true_got(ret == GEARMAN_SUCCESS, gearman_strerror(ret));
  test_truth(warning_success);

  gearman_client_set_warning_fn(client, NULL);
  gearman_task_free(task);

  return TEST_SUCCESS;
}