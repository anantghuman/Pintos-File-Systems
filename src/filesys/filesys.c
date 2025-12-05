#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "string.h"
#include "threads/thread.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void filesys_init (bool format)
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  if (format)
    do_format ();

  free_map_open ();
}

static bool subdir_path (char *name, struct dir **dir_out, char *file_name_out) {
  struct dir *directory;
  int len;
  char *temp = NULL;
  char part[NAME_MAX + 1];

  if (!name ||  name[0] == '\0') {
    return false;
  }

  directory = ((name[0] == '/') ? dir_open_root () : 
              dir_reopen (thread_current ()->curr_working_dir));
  
  if (!directory)
 {
    return false;
  }

  char *path = malloc (strlen (name) + 1); 
  if (!path) {
    dir_close (directory);
    return false;
  }

  strlcpy (path, name, strlen (name) + 1);
  file_name_out[0] = '\0';

  char *token = strtok_r (path, "/", &temp);
  while (token != NULL) {
    if (token[0] == '\0') {
      token = strtok_r (NULL, "/", &temp);
      continue;
    }

    strlcpy (part, token, strlen (token) + 1);


    if (temp && *temp != '\0')
      {
        struct inode *in = NULL;
        if (!dir_lookup (directory, part, &in) || !is_directory (in)) 
          {
            if (!is_directory (in)) 
              {
                inode_close (in);
              }
            dir_close (directory);
            free (path);
            return false;
          }

        struct dir *t = dir_open (in);
        dir_close(directory);
        directory = t;
      }
    else
      {
        strlcpy (file_name_out, part, NAME_MAX + 1);
        break;
      }
    token = strtok_r (NULL, "/", &temp);
  }

  if (file_name_out[0] == '\0') 
  {
    strlcpy (file_name_out, ".", NAME_MAX + 1);
  }
  
  *dir_out = directory;
  free (path);
  return true;

}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void filesys_done (void) { free_map_close (); }

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool filesys_create (const char *name, off_t initial_size)
{
  block_sector_t inode_sector = 0;
  struct dir *dir = dir_open_root ();
  char fname[NAME_MAX + 1];
  if (!subdir_path ((char*) name, &dir, fname))
    {
      return false;
    }
  bool success = (dir != NULL && free_map_allocate (1, &inode_sector) &&
                  inode_create (inode_sector, initial_size, false) &&
                  dir_add (dir, fname, inode_sector));
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);
    
  dir_close (dir);
  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *filesys_open (const char *name)
{
  struct dir *dir = dir_open_root ();
  struct inode *inode = NULL;
  char fname[NAME_MAX + 1];
  if (!subdir_path (name, &dir, fname))
    {
      return NULL;
    }
  if (!dir_lookup (dir, fname, &inode))
    {
      dir_close (dir);
      return NULL;
    }
  dir_close (dir);
  return is_directory(inode) ? dir_open(inode) : file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool filesys_remove (const char *name)
{
  struct dir *dir = dir_open_root ();
  char fname[NAME_MAX + 1];
  if (!subdir_path (name, &dir, fname))
    {
      return false;
    }
  bool success = dir != NULL && dir_remove (dir, fname);
  dir_close (dir);

  return success;
}

/* Formats the file system. */
static void do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
