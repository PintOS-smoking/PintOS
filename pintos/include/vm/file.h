#ifndef VM_FILE_H
#define VM_FILE_H
#include <list.h>
#include "filesys/file.h"

struct page;
enum vm_type;
struct mmap_file;

struct file_page {
    struct file *file;
    off_t ofs;
    size_t read_bytes;
    size_t zero_bytes;
    struct mmap_file *mmap;   
};

struct mmap_file {
    void *start;
    size_t page_cnt;
    struct file *file;
    off_t offset;
    struct list_elem elem;
};


void vm_file_init (void);
bool file_backed_initializer (struct page *page, enum vm_type type, void *kva);
bool lazy_load_file (struct page *page, void *aux);
void *do_mmap(void *addr, size_t length, int writable, struct file *file, off_t offset);
void do_munmap (void *va);
#endif
