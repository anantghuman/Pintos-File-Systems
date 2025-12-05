#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
{
  off_t length;         /* File size in bytes. */
  unsigned magic;       /* Magic number. */
  bool is_dir;
  uint32_t unused[111]; /* Not used. */

  block_sector_t direct_blocks[12];
  block_sector_t indirect_block;
  block_sector_t double_indirect_block;

  
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode
{
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
  struct inode_disk data; /* Inode content. */
  struct lock inode_lock;
};

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */

   static block_sector_t get_data_block (struct inode_disk *inode_d, size_t index, bool allocate);
static block_sector_t byte_to_sector (const struct inode *inode, off_t pos)
{
  ASSERT (inode != NULL);
  if (pos < inode->data.length)
    return get_data_block (&inode->data, pos / BLOCK_SECTOR_SIZE, false);
  else
    return -1;
}

static block_sector_t get_data_block (struct inode_disk *inode_d, size_t index, bool allocate) {
  static char zero_block[BLOCK_SECTOR_SIZE];
  if (index < DIRECT_BLOCKS_COUNT) {
    if (inode_d->direct_blocks[index] == 0 && allocate) 
      {
        if (!free_map_allocate (1, &inode_d->direct_blocks[index]))
          return -1;
        block_write (fs_device, inode_d->direct_blocks[index], zero_block);
      }
      return inode_d->direct_blocks[index] ? inode_d->direct_blocks[index] : -1;
  }
  index -= DIRECT_BLOCKS_COUNT;
  if (index < BLOCK_SECTOR_SIZE/ sizeof (block_sector_t)) 
    {
      block_sector_t indirect = inode_d->indirect_block;
      if (indirect == 0 && allocate) 
        {
          if (!free_map_allocate (1, &inode_d->indirect_block))
          {
            return -1;
          }
          indirect = inode_d->indirect_block;
          block_write (fs_device, inode_d->indirect_block, zero_block);
        }
      if (indirect == 0 && !allocate) 
        {
          return -1;
        }
      block_sector_t *indirect_data = malloc (BLOCK_SECTOR_SIZE);
      if (indirect_data == NULL) 
        {
          return -1;
        }
      block_read (fs_device, indirect, indirect_data);
      if (indirect_data[index] == 0)
        {
          if (allocate)
          {
            if (!free_map_allocate (1, &indirect_data[index]))
            {
              free (indirect_data);
              return -1;
            }
            block_write (fs_device, indirect_data[index], zero_block);
            block_write (fs_device, indirect, indirect_data);
          }
        }
        
      block_sector_t ret = 
      indirect_data[index] ? indirect_data[index] : -1;
        
      free (indirect_data);
      return ret;
    }
    index -= BLOCK_SECTOR_SIZE/ sizeof (block_sector_t);
    size_t index1 = index / (BLOCK_SECTOR_SIZE/ sizeof (block_sector_t));
    size_t index2 = index % (BLOCK_SECTOR_SIZE/ sizeof (block_sector_t));
    if (index1 >= BLOCK_SECTOR_SIZE/ sizeof (block_sector_t)) 
      {
        return -1;
      }
    if (inode_d->double_indirect_block == 0 && allocate)
      {    
        if (!free_map_allocate (1, &inode_d->double_indirect_block))
          {
            return -1;
          }
        block_write (fs_device, inode_d->double_indirect_block, zero_block);
      }
    if (inode_d->double_indirect_block == 0 && !allocate)
      {
        return -1;
      }
    block_sector_t *index1_level = malloc (BLOCK_SECTOR_SIZE);
    if (index1_level == NULL)
      {
        return -1;
      }
    block_read (fs_device, inode_d->double_indirect_block, index1_level);
    block_sector_t index1_sector = index1_level[index1];
    if (index1_sector == 0 && allocate)
      {
          if (!free_map_allocate (1, &index1_level[index1]))
            {
              free (index1_level);
              return -1;
            }
          index1_sector = index1_level[index1];
          block_write (fs_device, index1_sector, zero_block);
          block_write (fs_device, inode_d->double_indirect_block, index1_level);
      }
    block_sector_t *index2_level = malloc (BLOCK_SECTOR_SIZE);
    if (index2_level == NULL) {
      return -1;
    }
    block_read (fs_device, index1_sector, index2_level);
    if (index2_level[index2] == 0 && allocate) {
      if (!free_map_allocate(1, &index2_level[index2])) {
        free (index2_level);
        free (index1_level);
        return -1;
      }
      block_write (fs_device, index2_level[index2], zero_block);
      block_write (fs_device, index1_sector, index2_level);
    }
    block_sector_t ret = -1;
    if (index2_level[index2]) {
      ret = index2_level[index2];
    }
    free (index1_level);
    free (index2_level);
    return ret;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void inode_init (void) { list_init (&open_inodes); }

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool inode_create (block_sector_t sector, off_t length, bool is_dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  disk_inode->is_dir = is_dir;
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      if (length != 0)
        {
          size_t s = DIV_ROUND_UP (length, BLOCK_SECTOR_SIZE);
          for (int i = 0; i < s ; i++) {
            block_sector_t temp = get_data_block(disk_inode, i, true);
            if (temp == -1) {
              free (disk_inode);
              return success;
            }
          }
          success = true;
        }
      block_write (fs_device, sector, disk_inode);
      success = true;
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;
  

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e))
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector)
        {
          inode_reopen (inode);
          return inode;
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;
  lock_init(&inode->inode_lock);

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  block_read (fs_device, inode->sector, &inode->data);
  memcpy (&inode->data, &inode->data, sizeof (inode->data));
  return inode;
}

/* Reopens and returns INODE. */
struct inode *inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}


void inode_free_direct_blocks (struct inode *inode)
{
  // free direct blocks
  for (int i = 0; i < DIRECT_BLOCKS_COUNT; i++) 
  {
    if (inode->data.direct_blocks[i] != 0)
      free_map_release (inode->data.direct_blocks[i], 1);
  }
}

void inode_free_indirect_block (struct inode *inode)
{
  // free indirect blocks
  if (inode->data.indirect_block != 0)
  {
    block_sector_t *temp = malloc (sizeof (block_sector_t));
    block_read (fs_device, inode->data.indirect_block, temp);
    for (int i = 0; i < BLOCK_SECTOR_SIZE / sizeof (block_sector_t); i++) 
    {
      if (temp[i] != 0)
        free_map_release (temp[i], 1);
    }
    free (temp);
    free_map_release (inode->data.indirect_block, 1);
  }
}

void inode_free_double_indirect_block (struct inode *inode)
{
  // free double indirect blocks
  size_t size = BLOCK_SECTOR_SIZE / sizeof (block_sector_t);
  if (inode->data.double_indirect_block != 0)
    {
      {
        block_sector_t *level1 = malloc (BLOCK_SECTOR_SIZE);
        block_read (fs_device, inode->data.double_indirect_block, level1);

        for (int i = 0; i < size; i++)
        {
          inode_free_indirect_block(level1[i]);
        }
      }
    }
}

/* Closes INODE and writes it to disk. (Does it?  Check code.)
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void inode_close (struct inode *inode)
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);

      /* Deallocate blocks if removed. */
      if (inode->removed)
        {
          inode_free_direct_blocks (inode);
          inode_free_indirect_block (inode);
          inode_free_double_indirect_block (inode);
          free_map_release (inode->sector, 1);
        }

      free (inode);
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove (struct inode *inode)
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

bool is_directory (struct inode *i) {
  return i->data.is_dir;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at (struct inode *inode, void *buffer_, off_t size,
                     off_t offset)
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0)
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          block_read (fs_device, sector_idx, buffer + bytes_read);
        }
      else
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          block_read (fs_device, sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                      off_t offset)
{
  lock_acquire (&inode->inode_lock);
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt) {
    lock_release (&inode->inode_lock);
    return 0;
  }

  while (size > 0)
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = get_data_block (&inode->data, offset / BLOCK_SECTOR_SIZE, true);
      if (sector_idx == -1) {
        break;
      }
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int chunk_size = size;
      if (chunk_size >= sector_left)
        chunk_size = sector_left;


      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          block_write (fs_device, sector_idx, buffer + bytes_written);
        }
      else
        {
          /* We need a bounce buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left)
            block_read (fs_device, sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          block_write (fs_device, sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
      
      if (offset > inode->data.length)
      {
        inode->data.length = offset;
      }
    }
  free (bounce);
  block_write (fs_device, inode->sector, &inode->data);
  lock_release (&inode->inode_lock);
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write (struct inode *inode)
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write (struct inode *inode)
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length (const struct inode *inode) { return inode->data.length; }
