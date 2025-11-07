#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "pagedir.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

struct lock file_lock;

static void syscall_handler (struct intr_frame *);

void syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&file_lock);
}

/* Verifies that a pointer is valid, Otherwise returns -1*/
void check_ptr (const void *ptr) 
{
  if (ptr == NULL || !is_user_vaddr (ptr) || 
      pagedir_get_page (thread_current ()->pagedir, ptr) == NULL)
  {
    thread_current()->exit_stat = -1;
    printf("%s: exit(-1)\n", thread_current ()->name);
    thread_exit(); 
  }
}

/* Returns the file descriptor for a given FD. */
struct file_descriptor *find_filept(int fd) 
{ 
  struct thread *fd_ptr = thread_current ();
  struct list_elem *i = list_begin (&fd_ptr->fd_table);
  while (i != list_end (&fd_ptr->fd_table)) {
    struct file_descriptor *file_desc = 
    list_entry (i, struct file_descriptor, file_elem);
    if (file_desc->num_fd == fd) {
      return file_desc;
    }
    i = list_next (i);
    
  }
  return NULL;
}

//We all drove on this function but specifications are below in each case
//Explanation: Our goal within each switch case was to validate all 
// pointers, strings, and buffers. We also made sure that we were using the 
// right calls for each syscall. We used locks whenever necessary to prevent 
// race conditions with the file system.
static void syscall_handler (struct intr_frame *f UNUSED)
{
  check_ptr (f->esp);
  check_ptr ((const int*) f->esp + 3);
  int *fd_ptr;  
  int *buf_ptr;
  int *size_ptr;
  int syscall_number = *(int *) f->esp;
  struct file_descriptor *file_desc;
  switch (syscall_number) {
    //Anant drove
    case SYS_HALT:
      shutdown_power_off ();
      break;
    //Soham drove
    case SYS_EXIT: {
      int* fd_ptr = (int *) f->esp + 1;
      check_ptr (fd_ptr);
      thread_current ()->exit_stat = *fd_ptr;  
      printf("%s: exit(%d)\n", thread_current ()->name, *fd_ptr);
      thread_exit ();
      break;
    }
    //Sai drove
    case SYS_EXEC: {
      char **fd_ptr = (char **)((int *) f->esp + 1);
      check_ptr (fd_ptr);
      check_ptr (*fd_ptr);
      char *executable = *fd_ptr;
      if (executable == NULL || *executable == '\0') 
      {
        f->eax = -1;
        break;
      }

      for (char* t = executable; ; t++) {
        check_ptr(t);
        if (*t == '\0') {
          break;
        }
      }
      tid_t tid = process_execute ((const char *) executable);
      struct child_process *ch = NULL;
      for (struct list_elem *c = list_begin(&thread_current ()->children); 
              c != list_end(&thread_current ()->children); c = list_next (c)) {
        struct child_process *child = list_entry (c, struct child_process,
                                                               child_elem);
        if (child->pid == tid) {
          ch = child;
          break;
        }
      }
      if (ch == NULL || tid == TID_ERROR) {
        f->eax = -1;
        break;
      }
      sema_down (&ch->load_wait);
      if (!ch->success) {
        tid = TID_ERROR;
        list_remove (&ch->child_elem);
      }
      if (tid == TID_ERROR) {
        f->eax = -1;
      } else {
        f->eax = tid;
      }
      break;
    }
    //Soham drove
    case SYS_WAIT: {
      int* fd_ptr = (int*) f->esp + 1;
      check_ptr (fd_ptr);
      f->eax = process_wait ((tid_t) *fd_ptr);
      break;
    }
    //Anant drove
    case SYS_CREATE: {
      char **fd_ptr = (char **) f->esp + 1;
      int *buf_ptr = (int *) f->esp + 2;
      check_ptr (fd_ptr);
      check_ptr (*fd_ptr);
      check_ptr (buf_ptr);
      
      char *file_name = *fd_ptr;
      if (file_name == NULL || *file_name == '\0') {
        thread_current ()->exit_stat = -1;
        printf("%s: exit(%d)\n", thread_current ()->name, -1);
        thread_exit ();
      }
      
      char *t = file_name;
      while (*t != '\0') {
        check_ptr(t);
        t++;
      }

      lock_acquire (&file_lock);
      f->eax = filesys_create ((const char*) *fd_ptr, (unsigned) *buf_ptr);
      lock_release (&file_lock);
      break;
    }
    //Soham drove
    case SYS_REMOVE:
      int *fd_ptr = (int *) f->esp + 1;
      check_ptr (fd_ptr);
      check_ptr ((const char *) *fd_ptr);
      lock_acquire (&file_lock);
      f->eax = filesys_remove ((const char *) *fd_ptr);
      lock_release (&file_lock);
      break;
    //Anant drove
    case SYS_OPEN: {
      char **fd_ptr = (char **)(int *) f->esp + 1;
      check_ptr (fd_ptr);
      char *file_name = *fd_ptr;

      if (file_name == NULL) {
        f->eax = -1;
        break;
      }
      check_ptr (*fd_ptr);
      if (*file_name == '\0') {
        f->eax = -1;
        break;
      }

      char *ch = file_name;
      while (*ch != '\0') {
        check_ptr (ch);
        ch++;
      }
      check_ptr (file_name);
      lock_acquire (&file_lock);
      struct file *file = filesys_open ((const char *) *fd_ptr);
      if (!file) {
        f->eax = -1;
        lock_release (&file_lock);
        break;
      }
      file_desc = malloc (sizeof (*file_desc));
      file_desc->num_fd = thread_current ()->current_fd++;
      file_desc->file = file;
      f->eax = file_desc->num_fd;
      list_push_back (&thread_current ()->fd_table, &file_desc->file_elem);
      f->eax = file_desc->num_fd;
      lock_release (&file_lock);
      break;
    }
    //Anant drove
    case SYS_FILESIZE:
      fd_ptr = (int *) f->esp + 1;
      check_ptr (fd_ptr);

      lock_acquire (&file_lock);
      file_desc = find_filept (*fd_ptr);
      if (!file_desc) {
        f->eax = -1;
      } else {
        f->eax = file_length (file_desc->file);
      }
      lock_release (&file_lock);
      break;
    // Soham drove
    case SYS_READ:
      fd_ptr = (int *) f->esp + 1;
      buf_ptr = (void **) ((int*) f->esp + 2);
      size_ptr = (int *) f->esp + 3;
      check_ptr (fd_ptr);
      check_ptr (buf_ptr);
      check_ptr (size_ptr);

      if (*size_ptr < 0) {
        f->eax = -1;
        break;
      }
      if (*size_ptr > 0) {
        check_ptr (*buf_ptr);
      }
      if (*fd_ptr == 0) {
        uint8_t *t = (uint8_t*) *buf_ptr;
        int i = 0;
        while (i < *size_ptr) {
          check_ptr (t + i);
          t[i] = input_getc ();
          i++;
        }
        f->eax = *size_ptr;
        break;
      }

      if (*fd_ptr == 1) {
        f->eax = -1;
        break;
      }
    
      lock_acquire (&file_lock);
      file_desc = find_filept (*fd_ptr);
      if (file_desc == NULL) {
        f->eax = -1;
        lock_release (&file_lock);
        break;
      }
      f->eax = file_read (file_desc->file, *buf_ptr, *size_ptr);
      lock_release (&file_lock);
      break;
    //Soham drove
    case SYS_WRITE: {
      int *t = (int *) f->esp + 1;
      void **t2 = (void **)(int *) f->esp + 2;
      unsigned *t3 = (unsigned *) f->esp + 3;
      check_ptr (t);
      check_ptr (t2);
      check_ptr (t3);
      
      if (*t3 > 0) {
        check_ptr (*t2);
      }

      if (*t == 1) {
        f->eax = *t3;
        putbuf ((const char *) *t2, *t3);
        break;
      }

      if (*t == 0) {
        f->eax = -1;
        break;
      }

      lock_acquire (&file_lock);
      file_desc = find_filept (*t);
      if (file_desc == NULL) {
        f->eax = -1;
        lock_release (&file_lock);
        break;
      }
      f->eax = file_write (file_desc->file, *t2, *t3);
      lock_release (&file_lock);
      break;
    }
    //Soham drove
    case SYS_SEEK:
      fd_ptr = (int *) (f->esp) + 1;
      unsigned *t2 = (unsigned *)(int *) (f->esp) + 2;
      check_ptr (fd_ptr);
      check_ptr (t2);
      lock_acquire (&file_lock);
      file_desc = find_filept (*fd_ptr);
      if (file_desc != NULL) {
        file_seek (file_desc->file, *t2);
      } 
      lock_release (&file_lock);
      break;
    //Soham drove
    case SYS_TELL:
      fd_ptr = (int *) (f->esp) + 1;
      check_ptr (fd_ptr);
      lock_acquire (&file_lock);
      file_desc = find_filept (*fd_ptr);
      if (file_desc) {
        f->eax = file_tell (file_desc->file);
      } else {
        f->eax = -1;
      }
      lock_release (&file_lock);
      break;
    //Soham drove
    case SYS_CLOSE:
      fd_ptr = (int *) (f->esp) + 1;
      check_ptr (fd_ptr);
      if (*fd_ptr < 2) {
        break;
      }
      lock_acquire (&file_lock);
      file_desc = find_filept (*fd_ptr);
      if (file_desc) {
        file_close (file_desc->file);
        list_remove (&file_desc->file_elem);
        free (file_desc);
      }
      lock_release (&file_lock);
      break;
  }
  // thread_current()->status = -1;
  // thread_exit();
}
