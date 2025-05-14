#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* A single directory entry. */
struct dir_entry 
{
  block_sector_t inode_sector;        /* Sector number of header. */
  char name[NAME_MAX + 1];            /* Null terminated file name. */
  bool in_use;                        /* In use or free? */
};

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (block_sector_t sector, size_t entry_cnt)
{
  return inode_create (sector, entry_cnt * sizeof (struct dir_entry), true);
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (struct inode *dinode, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dinode != NULL);
  ASSERT (name != NULL);

  for (ofs = 0; inode_read_at (dinode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e)
    {
      if (e.in_use && !strcmp (name, e.name)) 
        {
          if (ep != NULL)
            *ep = e;
          if (ofsp != NULL)
            *ofsp = ofs;
          return true;
        }
    }
  return false;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
   bool
   dir_lookup (struct inode *dir_inode, const char *name, struct inode **result_inode) {
       ASSERT(dir_inode != NULL);
       ASSERT(name != NULL);
       ASSERT(result_inode != NULL);
   
       struct dir_entry entry;
       off_t offset = 0;
       /* Dizin içerisindeki her girdi bloğunu tara */
       while (inode_read_at(dir_inode, &entry, sizeof entry, offset) == sizeof entry) {
           if (entry.in_use && !strcmp(name, entry.name)) {
               /* Aranan isim bulundu, ilgili inode'u aç */
               *result_inode = inode_open(entry.inode_sector);
               return (*result_inode != NULL);
           }
           offset += sizeof entry;
       }
       /* Bulunamadı */
       *result_inode = NULL;
       return false;
   }
   
/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct inode *dir_inode, const char *name, block_sector_t inode_sector) {
    ASSERT(dir_inode != NULL);
    ASSERT(name != NULL);
    struct dir_entry new_entry;
    /* İsim uzunluk kontrolü: eğer çok uzunsa eklenemez */
    if (strlen(name) > NAME_MAX) {
        return false;
    }
    /* Aynı isim zaten var mı? */
    struct inode *tmp_inode = NULL;
    if (dir_lookup(dir_inode, name, &tmp_inode)) {
        /* Varsa, yeni bir inode açılmış olabilir, kapatalım */
        inode_close(tmp_inode);
        return false;
    }
    /* Boş bir giriş yeri bul veya dizin sonuna ekle */
    struct dir_entry entry;
    off_t ofs;
    for (ofs = 0; inode_read_at(dir_inode, &entry, sizeof entry, ofs) == sizeof entry; ofs += sizeof entry) {
        if (!entry.in_use) {
            break;  /* Kullanılmayan bir slot bulundu */
        }
    }
    /* Yeni girdi yapılandırması */
    new_entry.inode_sector = inode_sector;
    new_entry.in_use = true;
    strlcpy(new_entry.name, name, sizeof new_entry.name);
    /* Yeni girdiyi dizine yaz */
    return inode_write_at(dir_inode, &new_entry, sizeof new_entry, ofs) == sizeof new_entry;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct inode *dir_inode, const char *name) {
    ASSERT(dir_inode != NULL);
    ASSERT(name != NULL);
    struct dir_entry entry;
    off_t ofs;
    bool found = false;
    for (ofs = 0; inode_read_at(dir_inode, &entry, sizeof entry, ofs) == sizeof entry; ofs += sizeof entry) {
        if (entry.in_use && !strcmp(entry.name, name)) {
            found = true;
            break;
        }
    }
    if (!found) {
        return false; 
    }
   
    entry.in_use = false;
    if (inode_write_at(dir_inode, &entry, sizeof entry, ofs) != sizeof entry) {
        return false; /* Yazma hatası */
    }
    return true;
}
   
bool
dir_readdir (struct file *dir_file, char name[NAME_MAX + 1]) {
    ASSERT(dir_file != NULL);
    struct dir_entry entry;
    if (file_tell(dir_file) == 0) {
        file_seek(dir_file, 2 * sizeof entry);
    }
    while (file_read(dir_file, &entry, sizeof entry) == sizeof entry) {
        if (entry.in_use) {
            strlcpy(name, entry.name, NAME_MAX + 1);
            return true;
        }
    }
    return false; /* Başka giriş kalmadı */
}

bool
dir_is_empty (struct inode *dir_inode) {
    struct dir_entry entry;
    off_t ofs = 0;
    /* İlk iki özel girişi atla */
    ofs += 2 * sizeof entry;
    /* Bu noktadan sonra bir tane bile in_use girişi bulunursa boş değildir */
    while (inode_read_at(dir_inode, &entry, sizeof entry, ofs) == sizeof entry) {
        if (entry.in_use) {
            return false;
        }
        ofs += sizeof entry;
    }
    return true;
}
