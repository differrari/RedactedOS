#pragma once

#include "files/jobs.h"
#include "process/process.h"

job_id_t create_new_job(job_application_t application);
void fulfill_job(job_id_t job_id, thread_t *thread);
