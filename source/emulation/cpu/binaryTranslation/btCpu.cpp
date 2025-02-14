#include "boxedwine.h"
#include "btCodeChunk.h"
#include "btCpu.h"
#include "btData.h"
#include "ksignal.h"
#include "knativethread.h"
#include "knativesystem.h"
#include "../../hardmmu/hard_memory.h"

#ifdef BOXEDWINE_BINARY_TRANSLATOR

typedef void (*StartCPU)();

void BtCPU::run() {
    while (true) {
        this->memOffset = this->thread->process->memory->id;
        this->exitToStartThreadLoop = 0;
        if (setjmp(this->runBlockJump) == 0) {
            StartCPU start = (StartCPU)this->init();
            start();
#ifdef __TEST
            return;
#endif
        }
        if (this->thread->terminating) {
            break;
        }
        if (this->exitToStartThreadLoop) {
            Memory* previousMemory = this->thread->process->previousMemory;
            if (previousMemory && previousMemory->getRefCount() == 1) {
                // :TODO: this seem like a bad dependency that memory will access KThread::currentThread()
                Memory* currentMemory = this->thread->process->memory;
                this->thread->process->memory = previousMemory;
                this->thread->memory = previousMemory;
                delete previousMemory;
                this->thread->process->memory = currentMemory;
                this->thread->memory = currentMemory;
            }
            else {
                previousMemory->decRefCount();
            }
            this->thread->process->previousMemory = NULL;
        }
        if (this->inException) {
            this->inException = false;
        }
    }
}

std::shared_ptr<BtCodeChunk> BtCPU::translateChunk(U32 ip) {
    std::shared_ptr<BtData> firstPass = createData();
    firstPass->ip = ip;
    firstPass->startOfDataIp = ip;
    translateData(firstPass);

    std::shared_ptr<BtData> secondPass = createData();
    secondPass->ip = ip;
    secondPass->startOfDataIp = ip;
    secondPass->calculatedEipLen = firstPass->ip - firstPass->startOfDataIp;
    translateData(secondPass, firstPass);
    S32 failedJumpOpIndex = this->preLinkCheck(secondPass.get());

    if (failedJumpOpIndex == -1) {
        std::shared_ptr<BtCodeChunk> chunk = secondPass->commit(false);
        link(secondPass, chunk);
        return chunk;
    }
    else {
        firstPass = createData();
        firstPass->ip = ip;
        firstPass->startOfDataIp = ip;
        firstPass->stopAfterInstruction = failedJumpOpIndex;
        translateData(firstPass);

        secondPass = createData();
        secondPass->ip = ip;
        secondPass->startOfDataIp = ip;
        secondPass->calculatedEipLen = firstPass->ip - firstPass->startOfDataIp;
        secondPass->stopAfterInstruction = failedJumpOpIndex;
        translateData(secondPass, firstPass);

        std::shared_ptr<BtCodeChunk> chunk = secondPass->commit(false);
        link(secondPass, chunk);
        return chunk;
    }
}

U64 BtCPU::reTranslateChunk() {
#ifndef __TEST
    // only one thread at a time can update the host code pages and related date like opToAddressPages
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(this->thread->memory->executableMemoryMutex);
#endif
    std::shared_ptr<BtCodeChunk> chunk = this->thread->memory->getCodeChunkContainingEip(this->eip.u32 + this->seg[CS].address);
    if (chunk) {
        chunk->releaseAndRetranslate();
    }

    U64 result = (U64)this->thread->memory->getExistingHostAddress(this->eip.u32 + this->seg[CS].address);
    if (result == 0) {
        result = (U64)this->translateEip(this->eip.u32);
    }
    if (result == 0) {
        kpanic("BtCPU::reTranslateChunk failed to translate code in exception");
    }
    return result;
}

U64 BtCPU::handleChangedUnpatchedCode(U64 rip) {
#ifndef __TEST
    // only one thread at a time can update the host code pages and related date like opToAddressPages
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(this->thread->memory->executableMemoryMutex);
#endif

    unsigned char* hostAddress = (unsigned char*)rip;
    std::shared_ptr<BtCodeChunk> chunk = this->thread->memory->getCodeChunkContainingHostAddress(hostAddress);
    if (!chunk) {
        kpanic("BtCPU::handleChangedUnpatchedCode: could not find chunk");
    }
    U32 startOfEip = chunk->getEipThatContainsHostAddress(hostAddress, NULL, NULL);
    if (!chunk->isDynamicAware() || !chunk->retranslateSingleInstruction(this, hostAddress)) {
        chunk->releaseAndRetranslate();
    }
    U64 result = (U64)this->thread->memory->getExistingHostAddress(startOfEip);
    if (result == 0) {
        result = (U64)this->translateEip(startOfEip - this->seg[CS].address);
    }
    if (result == 0) {
        kpanic("BtCPU::handleChangedUnpatchedCode failed to translate code in exception");
    }
    return result;
}

U64 BtCPU::handleIllegalInstruction(U64 rip) {
    if (*((U8*)rip) == 0xce) {
        return this->handleChangedUnpatchedCode(rip);
    }
    if (*((U8*)rip) == 0xcd) {
        // free'd chunks are filled in with 0xcd, if this one is free'd, it is possible another thread replaced the chunk
        // while this thread jumped to it and this thread waited in the critical section at the top of this function.
        void* host = this->thread->memory->getExistingHostAddress(this->eip.u32 + this->seg[CS].address);
        if (host) {
            return (U64)host;
        }
        else {
            kpanic("BtCPU::handleIllegalInstruction tried to run code in a free'd chunk");
        }
    }
    kpanic("BtCPU::handleIllegalInstruction instruction %X not handled", *(U32*)rip);
    return 0;
}

static U8 fetchByte(U32* eip) {
    return readb((*eip)++);
}

DecodedOp* BtCPU::getOp(U32 eip, bool existing) {
    if (this->isBig()) {
        eip += this->seg[CS].address;
    }
    else {
        eip = this->seg[CS].address + (eip & 0xFFFF);
    }
    if (!existing || this->thread->memory->getExistingHostAddress(eip)) {
        THREAD_LOCAL static DecodedBlock* block;
        if (!block) {
            block = new DecodedBlock();
        }
        decodeBlock(fetchByte, eip, this->isBig(), 4, 64, 1, block);
        return block->op;
    }
    return NULL;
}

void* BtCPU::translateEipInternal(U32 ip) {
    if (!this->isBig()) {
        ip = ip & 0xFFFF;
    }
    U32 address = this->seg[CS].address + ip;
    void* result = this->thread->memory->getExistingHostAddress(address);

    if (!result) {
        std::shared_ptr<BtCodeChunk> chunk = this->translateChunk(ip);
        result = chunk->getHostAddress();
        chunk->makeLive();
    }
    return result;
}

void* BtCPU::translateEip(U32 ip) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(this->thread->memory->executableMemoryMutex);

    void* result = translateEipInternal(ip);
    makePendingCodePagesReadOnly();
    return result;
}

void BtCPU::makePendingCodePagesReadOnly() {
    for (int i = 0; i < (int)this->pendingCodePages.size(); i++) {
        // the chunk could cross a page and be a mix of dynamic and non dynamic code
        if (this->thread->memory->dynamicCodePageUpdateCount[this->pendingCodePages[i]] != MAX_DYNAMIC_CODE_PAGE_COUNT) {
            this->thread->memory->makeCodePageReadOnly(this->pendingCodePages[i]);
        }
    }
    this->pendingCodePages.clear();
}

void BtCPU::markCodePageReadOnly(BtData* data) {
    U32 pageStart = this->thread->memory->getNativePage((data->startOfDataIp + this->seg[CS].address) >> K_PAGE_SHIFT);
    if (pageStart == 0) {
        return; // x64CPU::init()
    }
    U32 pageEnd = this->thread->memory->getNativePage((data->ip + this->seg[CS].address - 1) >> K_PAGE_SHIFT);
    S32 pageCount = pageEnd - pageStart + 1;

    for (int i = 0; i < pageCount; i++) {
        pendingCodePages.push_back(pageStart + i);
    }  
}

U64 BtCPU::startException(U64 address, bool readAddress) {
    if (this->thread->terminating) {
        return (U64)this->returnToLoopAddress;
    }
    if (this->inException) {
        this->thread->seg_mapper((U32)address, readAddress, !readAddress);
        U64 result = (U64)this->translateEip(this->eip.u32);
        if (result == 0) {
            kpanic("Armv8btCPU::startException failed to translate code in exception");
        }
        return result;
    }
    return 0;
}

U64 BtCPU::handleFpuException(int code) {
    if (code == K_FPE_INTDIV) {
        this->prepareException(EXCEPTION_DIVIDE, 0);
    }
    else if (code == K_FPE_INTOVF) {
        this->prepareException(EXCEPTION_DIVIDE, 1);
    }
    else {
        this->prepareFpuException(code);
    }
    U64 result = (U64)this->translateEip(this->eip.u32);
    if (result == 0) {
        kpanic("Armv8btCPU::handleFpuException failed to translate code");
    }
    return result;
}

U32 dynamicCodeExceptionCount;

U64 BtCPU::handleAccessException(U64 ip, U64 address, bool readAddress) {
    if ((address & 0xFFFFFFFF00000000l) == this->thread->memory->id) {
        U32 emulatedAddress = (U32)address;
        U32 page = emulatedAddress >> K_PAGE_SHIFT;
        Memory* m = this->thread->memory;
        U32 flags = m->flags[page];
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(this->thread->memory->executableMemoryMutex);

        // do we need to dynamicly grow the stack?
        if (page >= this->thread->stackPageStart && page < this->thread->stackPageStart + this->thread->stackPageCount) {
            U32 startPage = m->getEmulatedPage(m->getNativePage(page - INITIAL_STACK_PAGES)); // stack grows down
            U32 endPage = this->thread->stackPageStart + this->thread->stackPageCount - this->thread->stackPageSize;
            U32 pageCount = endPage - startPage;
            BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(m->pageMutex);
            m->allocPages(startPage, pageCount, PAGE_READ | PAGE_WRITE, 0, 0, 0);
            this->thread->stackPageSize += pageCount;
            return 0;
        }
        std::shared_ptr<BtCodeChunk> chunk = m->getCodeChunkContainingHostAddress((void*)ip);
        if (chunk) {
            this->eip.u32 = chunk->getEipThatContainsHostAddress((void*)ip, NULL, NULL) - this->seg[CS].address;
            // if the page we are trying to access needs a special memory offset and this instruction isn't flagged to looked at that special memory offset, then flag it
            if (flags & PAGE_MAPPED_HOST && (((flags & PAGE_READ) && readAddress) || ((flags & PAGE_WRITE) && !readAddress))) {
                m->setNeedsMemoryOffset(getEipAddress());
                DecodedOp* op = this->getOp(this->eip.u32, true);
                handleStringOp(op); // if we were in the middle of a string op, then reset RSI and RDI so that we can re-enter the same op
                chunk->releaseAndRetranslate();
                return getIpFromEip();
            }
            else {
                if (!readAddress && (flags & PAGE_WRITE) && !(this->thread->memory->nativeFlags[this->thread->memory->getNativePage(page)] & NATIVE_FLAG_CODEPAGE_READONLY)) {
                    // :TODO: this is a hack, why is it necessary
                    m->updatePagePermission(page, 1);
                    return 0;
                }
                else {
                    int ii = 0;
                }
            }
        }
    }

    U32 inst = *((U32*)ip);

    if (inst == largeAddressJumpInstruction && this->thread->memory->isValidReadAddress((U32)this->destEip, 1)) { // useLargeAddressSpace = true
        return (U64)this->translateEip((U32)this->destEip - this->seg[CS].address);
    }
    else if ((inst == pageJumpInstruction || inst == pageOffsetJumpInstruction) && (this->regPage || this->regOffset)) { // if these constants change, update handleMissingCode too     
        return this->handleMissingCode((U32)this->regPage, (U32)this->regOffset); // useLargeAddressSpace = false
    }
    else if (inst == 0xcdcdcdcd) {
        // this thread was waiting on the critical section and the thread that was currently in this handler removed the code we were running
        void* host = this->thread->memory->getExistingHostAddress(this->eip.u32 + this->seg[CS].address);
        if (host) {
            return (U64)host;
        }
        else {
            U64 result = (U64)this->translateEip(this->eip.u32);
            if (!result) {
                kpanic("x64CPU::handleAccessException tried to run code in a free'd chunk");
            }
            return result;
        }
    }
    else {
        // check if the emulated memory caused the exception
        if ((address & 0xFFFFFFFF00000000l) == this->thread->memory->id) {
            U32 emulatedAddress = (U32)address;
            U32 page = emulatedAddress >> K_PAGE_SHIFT;
            // check if emulated memory that caused the exception is a page that has code
            if (this->thread->memory->nativeFlags[this->thread->memory->getNativePage(page)] & NATIVE_FLAG_CODEPAGE_READONLY) {
                dynamicCodeExceptionCount++;
                return this->handleCodePatch(ip, emulatedAddress);
            }
            Memory* m = this->thread->memory;
            U32 nativeFlags = m->nativeFlags[m->getNativePage(page)];
            if ((flags & PAGE_MAPPED_HOST) || (flags & (PAGE_READ | PAGE_WRITE)) != (nativeFlags & (PAGE_READ | PAGE_WRITE))) {
                int ii=0;
            }
        }

        // this can be exercised with Wine 5.0 and CC95 demo installer, it is triggered in strlen as it tries to grow the stack
        this->thread->seg_mapper((U32)address, readAddress, !readAddress, false);
        U64 result = (U64)this->translateEip(this->eip.u32);
        if (result == 0) {
            kpanic("x64CPU::handleAccessException failed to translate code");
        }
        return result;
    }
}

extern U32 platformThreadCount;

void BtCPU::startThread() {
    jmp_buf jmpBuf;
    KThread::setCurrentThread(thread);

    // :TODO: hopefully this will eventually go away.  For now this prevents a signal from being generated which isn't handled yet
    KNativeThread::sleep(50);

    if (!setjmp(jmpBuf)) {
        this->jmpBuf = &jmpBuf;
        this->run();
    }
    std::shared_ptr<KProcess> process = thread->process;
    process->deleteThread(thread);

    platformThreadCount--;
    if (platformThreadCount == 0) {
        KSystem::shutingDown = true;
        KNativeSystem::postQuit();
    }
}

// called from another thread
void BtCPU::wakeThreadIfWaiting() {
    BoxedWineCondition* cond = thread->waitingCond;

    // wait up the thread if it is waiting
    if (cond) {
        cond->lock();
        cond->signal();
        cond->unlock();
    }
}

DecodedBlock* BtCPU::getNextBlock() {
    return NULL;
}

S32 BtCPU::preLinkCheck(BtData* data) {
    for (S32 i = 0; i < (S32)data->todoJump.size(); i++) {
        U32 eip = this->seg[CS].address + data->todoJump[i].eip;
        U8 size = data->todoJump[i].offsetSize;

        if (size == 4 && data->todoJump[i].sameChunk) {
            bool found = false;

            for (U32 ip = 0; ip < data->ipAddressCount; ip++) {
                if (data->ipAddress[ip] == eip) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return data->todoJump[i].opIndex;
            }
        }
    }
    return -1;
}

U64 BtCPU::getIpFromEip() {
    U32 a = this->getEipAddress();
    U64 result = (U64)this->thread->memory->getExistingHostAddress(a);
    if (!result) {
        this->translateEip(this->eip.u32);
        result = (U64)this->thread->memory->getExistingHostAddress(a);
    }
    if (result == 0) {
        kpanic("BtCPU::getIpFromEip failed to translate code");
    }
    return result;
}

U64 BtCPU::handleMissingCode(U32 page, U32 offset) {
    this->eip.u32 = ((page << K_PAGE_SHIFT) | offset) - this->seg[CS].address;
    return (U64)this->translateEip(this->eip.u32);
}

bool BtCPU::handleStringOp(DecodedOp* op) {
    return op->isStringOp();
}

// if the page we are writing to has code that we have cached, then it will be marked NATIVE_FLAG_CODEPAGE_READONLY
//
// 1) This function will clear the page of all cached code
// 2) Mark all the old cached code with "0xce" so that if the program tries to run it again, it will re-compile it
// 3) NATIVE_FLAG_CODEPAGE_READONLY will be removed
U64 BtCPU::handleCodePatch(U64 rip, U32 address) {
#ifndef __TEST
    // only one thread at a time can update the host code pages and related date like opToAddressPages
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(this->thread->memory->executableMemoryMutex);
#endif
    Memory* memory = this->thread->memory;
    U32 nativePage = memory->getNativePage(address >> K_PAGE_SHIFT);
    // get the emulated eip of the op that corresponds to the host address where the exception happened
    std::shared_ptr<BtCodeChunk> chunk = this->thread->memory->getCodeChunkContainingHostAddress((void*)rip);

    this->eip.u32 = chunk->getEipThatContainsHostAddress((void*)rip, NULL, NULL) - this->seg[CS].address;

    // make sure it wasn't changed before we got the executableMemoryMutex lock
    if (!(memory->nativeFlags[nativePage] & NATIVE_FLAG_CODEPAGE_READONLY)) {
        return getIpFromEip();
    }

    // get the emulated op that caused the write
    DecodedOp* op = this->getOp(this->eip.u32, true);
    if (op) {
        if (this->flags & DF) {
            this->df = -1;
        }
        else {
            this->df = 1;
        }
        U32 addressStart = address;
        U32 len = instructionInfo[op->inst].writeMemWidth / 8;

        // Fix string will set EDI and ESI back to their correct values so we can re-enter this instruction
        if (handleStringOp(op)) {
            if (op->repNotZero || op->repZero) {
                len = len * (isBig() ? THIS_ECX : THIS_CX);
            }

            if (this->df == 1) {
                addressStart = (this->isBig() ? THIS_EDI : THIS_DI) + this->seg[ES].address;
            }
            else {
                addressStart = (this->isBig() ? THIS_EDI : THIS_DI) + this->seg[ES].address + (instructionInfo[op->inst].writeMemWidth / 8) - len;
            }
        }
        U32 startPage = addressStart >> K_PAGE_SHIFT;
        U32 endPage = (addressStart + len - 1) >> K_PAGE_SHIFT;
        memory->clearHostCodeForWriting(memory->getNativePage(startPage), memory->getNativePage(endPage - startPage) + 1);
        op->dealloc(true);
        return getIpFromEip();
    }
    else {
        kpanic("Threw an exception from a host location that doesn't map to an emulated instruction");
    }
    return 0;
}

void terminateOtherThread(const std::shared_ptr<KProcess>& process, U32 threadId) {
    KThread* thread = process->getThreadById(threadId);        
    if (thread) {
        thread->terminating = true;
        ((BtCPU*)thread->cpu)->exitToStartThreadLoop = true;
        ((BtCPU*)thread->cpu)->wakeThreadIfWaiting();
    }

    while (true) {
        BOXEDWINE_CRITICAL_SECTION_WITH_CONDITION(process->threadRemovedCondition);
        if (!process->getThreadById(threadId)) {
            break;
        }
        BOXEDWINE_CONDITION_WAIT_TIMEOUT(process->threadRemovedCondition, 1000);
    }
}

void terminateCurrentThread(KThread* thread) {
    thread->terminating = true;
    ((BtCPU*)thread->cpu)->exitToStartThreadLoop = true;
}

void unscheduleThread(KThread* thread) {
}

#endif
