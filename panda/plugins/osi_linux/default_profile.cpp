#include "osi_linux.h"
#include "default_profile.h"

target_ptr_t default_get_current_task_struct(CPUState *cpu)
{
	target_ptr_t kernel_esp = panda_current_sp(cpu);
	target_ptr_t ts = get_task_struct(cpu, (kernel_esp & THREADINFO_MASK));
	return ts;
}

/**
 * @brief Retrieves the address of the following task_struct in the process list.
 *
 * XXX: Can now be implemented with IMPLEMENT_OFFSET_GETTER_2LN
 */
target_ptr_t default_get_task_struct_next(CPUState *env, target_ptr_t task_struct)
{
    target_ptr_t tasks = get_tasks(env, task_struct);

    if (!tasks) {
        return (target_ptr_t)NULL;
    }
    else {
        return tasks-ki.task.tasks_offset;
    }
}

/**
 * @brief Retrieves the thread group leader address from task_struct.
 */
IMPLEMENT_OFFSET_GET(get_group_leader, task_struct, target_ptr_t, ki.task.group_leader_offset, 0)
target_ptr_t default_get_group_leader(CPUState *cpu, target_ptr_t ts)
{
	return get_group_leader(cpu, ts);
}

/**
 * @brief Retrieves the array of file structs from the files struct.
 * The n-th element of the array corresponds to the n-th open fd.
 */
IMPLEMENT_OFFSET_GET2L(get_files_fds, files_struct, target_ptr_t, ki.fs.fdt_offset, target_ptr_t, ki.fs.fd_offset, 0);

target_ptr_t default_get_file_fds(CPUState *cpu, target_ptr_t files)
{
	return get_files_fds(cpu, files);
}

/* vim:set tabstop=4 softtabstop=4 noexpandtab: */
