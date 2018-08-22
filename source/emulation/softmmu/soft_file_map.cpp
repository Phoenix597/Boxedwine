/*
 *  Copyright (C) 2016  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include "boxedwine.h"

#ifdef BOXEDWINE_DEFAULT_MMU

#include "soft_memory.h"
#include "soft_file_map.h"
#include "soft_ro_page.h"
#include "soft_rw_page.h"
#include "soft_invalid_page.h"
#include "soft_wo_page.h"
#include "soft_no_page.h"
#include "soft_ram.h"

FilePage* FilePage::alloc(const BoxedPtr<MappedFile>& mapped, U32 index, U32 flags) {
    return new FilePage(mapped, index, flags);
}

// :TODO: what about sync'ing the writes back to the file?
void FilePage::ondemmandFile(U32 address) {
    Memory* memory = KThread::currentThread()->process->memory;
    U32 page = address >> K_PAGE_SHIFT;
    bool read = this->canRead() | this->canExec();
    bool write = this->canWrite();
    U8* ram=NULL;

    address = address & (~K_PAGE_MASK);
    if (!write) {
        ram = mapped->systemCacheEntry->data[this->index];                
        this->flags |= PAGE_SHARED_SYSTEM;
    } 
    if (!ram) {
        ram = ramPageAlloc();
        U64 pos = this->mapped->file->getPos();
        this->mapped->file->seek(((U64)this->index) << K_PAGE_SHIFT);
        U32 len = this->mapped->file->readNative(ram, K_PAGE_SIZE);
        this->mapped->file->seek(pos);
        if (!write)
            mapped->systemCacheEntry->data[this->index] = ram;
    }

    if (read && write) {
        memory->mmu[page] = RWPage::alloc(ram, address, this->flags);
    } else if (write) {
        memory->mmu[page] = WOPage::alloc(ram, address, this->flags);
    } else if (read) { 
        memory->mmu[page] = ROPage::alloc(ram, address, this->flags);
    } else {
        memory->mmu[page] = NOPage::alloc(ram, address, this->flags);
    }
    this->close();
}

U8 FilePage::readb(U32 address) {	
    ondemmandFile(address);	
    return ::readb(address);
}

void FilePage::writeb(U32 address, U8 value) {
    ondemmandFile(address);	
    ::writeb(address, value);
}

U16 FilePage::readw(U32 address) {
    ondemmandFile(address);	
    return ::readw(address);
}

void FilePage::writew(U32 address, U16 value) {
    ondemmandFile(address);	
    ::writew(address, value);
}

U32 FilePage::readd(U32 address) {
    ondemmandFile(address);	
    return ::readd(address);
}

void FilePage::writed(U32 address, U32 value) {
    ondemmandFile(address);	
    ::writed(address, value);
}


U8* FilePage::physicalAddress( U32 address) {
    ondemmandFile(address);
    return ::getPhysicalAddress(address);
}

#endif