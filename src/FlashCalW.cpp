///////////////////////////////////////////////////////////////////////////////
// BOSSA
//
// Copyright (C) 2011-2012 ShumaTech http://www.shumatech.com/
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
///////////////////////////////////////////////////////////////////////////////
#include "FlashCalW.h"

#include <assert.h>
#include <unistd.h>
#include <stdio.h>

#define EEFC_KEY        0x5a

#define CALW_FCR       (_regs + 0x00)
#define CALW_FCMD      (_regs + 0x04)
#define CALW_FSR       (_regs + 0x08)
#define CALW_FPR       (_regs + 0x0C)
#define CALW_FVR       (_regs + 0x10)


#define FSR_FRDY       (1<<0)
#define FSR_FLOCKE     (1<<2)
#define FSR_SECURITY   (1<<4)

#define FCMD_KEY       0xA5

#define CMD_WP         0x01 // Write page
#define CMD_EP         0x02 // Erase page
#define CMD_CPB        0x03 // Clear page buffer
#define CMD_LP         0x04 // Lock region containing page
#define CMD_UP         0x05 // Unlock region containing page
#define CMD_ERASEALL   0x06
#define CMD_SSB        0x09 // Set Security Fuses

#define CMD_WUP        0x0D // Write user page
#define CMD_EUP        0x0E // Erase user page

FlashCalW::FlashCalW(Samba& samba,
                     const std::string& name,
                     uint32_t addr,
                     uint32_t pages,
                     uint32_t size,
                     uint32_t lockRegions,
                     uint32_t sambaRegionSize,
                     uint32_t user,
                     uint32_t stack,
                     uint32_t regs,
                     bool canBrownout)
  : Flash(samba, name, addr, pages, size, 1, lockRegions, user, stack),
      _regs(regs), _canBrownout(canBrownout), _eraseAuto(true)
{
    assert(pages <= 1024);
    assert(lockRegions <= 32);

    uint32_t page_size = 32L << ((_samba.readWord(CALW_FPR) >> 8) & 0x07);
    assert(size == page_size);

    assert(sambaRegionSize % size == 0); // SAM-BA region should end on a page boundary
    _reservedPages = sambaRegionSize / size;
}

FlashCalW::~FlashCalW()
{
}

void
FlashCalW::eraseAll()
{
  throw FlashCmdError(); // Unsure if a full erase will also erase SAM-BA
  waitFSR();
  writeFCMD(CMD_ERASEALL, 0);
}

void
FlashCalW::eraseAuto(bool enable)
{
    _eraseAuto = enable;
}

bool
FlashCalW::isLocked()
{
    waitFSR();
    uint32_t lock_mask = readFSR() >> 16;
    // TODO: this mask probably always shows the lower 0x4000 as locked, and rightly so
    return lock_mask;
}

bool
FlashCalW::getLockRegion(uint32_t region)
{
    if (region >= _lockRegions)
        throw FlashRegionError();

    waitFSR();
    uint32_t lock_mask = readFSR() >> 16;
    return lock_mask & (1<<region);
}

void
FlashCalW::setLockRegion(uint32_t region, bool enable)
{
    uint32_t page;

    if (region >= _lockRegions)
        throw FlashRegionError();

    if (enable != getLockRegion(region))
    {
      page = region * _pages / _lockRegions;
      if(!enable && _reservedPages && page < _reservedPages)
        throw FlashPageError(); // TODO: make proper error
      waitFSR();
      writeFCMD(enable ? CMD_LP : CMD_UP, page);
    }
}

bool
FlashCalW::getSecurity()
{
    waitFSR();
    return readFSR() & FSR_SECURITY;
}

void
FlashCalW::setSecurity()
{
    waitFSR();
    writeFCMD(CMD_SSB, 0);
}

bool
FlashCalW::getBod()
{
    if (!_canBrownout)
        return false;

    // Not implemented yet. On ATSAM4L These values live in the Flash user page anyhow
    return false;
}

void
FlashCalW::setBod(bool enable)
{
    if (!_canBrownout)
        return;
    // Not implemented yet. On ATSAM4L These values live in the Flash user page anyhow
}

bool
FlashCalW::getBor()
{
    if (!_canBrownout)
        return false;

    // Not implemented yet. On ATSAM4L These values live in the Flash user page anyhow
    return false;
}

void
FlashCalW::setBor(bool enable)
{
    if (!_canBrownout)
        return;

    // Not implemented yet. On ATSAM4L These values live in the Flash user page anyhow
}

bool
FlashCalW::getBootFlash()
{
  return false; // Not implemented on ATSAM4L
}

void
FlashCalW::setBootFlash(bool enable)
{
  // Not implemented on ATSAM4L
}

uint32_t
FlashCalW::getWritePageCommand()
{
  return CMD_WP;
}

uint32_t FlashCalW::getErasePageCommand()
{
  return CMD_EP;
}

void
FlashCalW::writePage(uint32_t page)
{
    if (page >= _pages)
        throw FlashPageError();

    if(page < _reservedPages)
      throw FlashPageError(); // TODO: Make a proper error message

    if(_eraseAuto) {
      waitFSR();
      writeFCMD(getErasePageCommand(), page);
    }

    waitFSR();
    writeFCMD(CMD_CPB, 0);

    waitFSR();
    _wordCopy.setDstAddr(_addr + page * _size);
    _wordCopy.setSrcAddr(_onBufferA ? _pageBufferA : _pageBufferB);
    _onBufferA = !_onBufferA;
    _wordCopy.runv();

    waitFSR();
    writeFCMD(getWritePageCommand(), page);
}

void
FlashCalW::readPage(uint32_t page, uint8_t* data)
{
    if (page >= _pages)
        throw FlashPageError();

    // The SAM3 firmware has a bug where it returns all zeros for reads
    // directly from the flash so instead, we copy the flash page to
    // SRAM and read it from there.
    // TODO: test if this is the case for SAM4 too
    _wordCopy.setDstAddr(_onBufferA ? _pageBufferA : _pageBufferB);
    _wordCopy.setSrcAddr(_addr + page * _size);
    waitFSR();
    _wordCopy.runv();
    _samba.read(_onBufferA ? _pageBufferA : _pageBufferB, data, _size);
}

void
FlashCalW::waitFSR()
{
    uint32_t tries = 0;
    uint32_t fsr;

    while (++tries <= 500)
    {
      fsr = readFSR();
      if (fsr & FSR_FLOCKE)
        throw FlashLockError();
      if (fsr & FSR_FRDY)
        break;
      usleep(100);
    }
    if (tries > 500)
      throw FlashCmdError();
}

void
FlashCalW::writeFCMD(uint8_t cmd, uint16_t page)
{
  _samba.writeWord(CALW_FCMD, (FCMD_KEY << 24) | (((uint32_t)page) << 8) | cmd);
}

uint32_t FlashCalW::readFSR()
{
  return _samba.readWord(CALW_FSR);
}


/* FlashCalWUserPage */

FlashCalWUserPage::FlashCalWUserPage(Samba& samba,
                  const std::string& name,
                  uint32_t addr,
                  uint32_t pages,
                  uint32_t size,
                  uint32_t user,
                  uint32_t stack,
                  uint32_t regs) :
  FlashCalW(samba, name, addr, pages, size, 0, 0, user, stack, regs, false)
{

}

FlashCalWUserPage::~FlashCalWUserPage()
{

}


void
FlashCalWUserPage::eraseAll()
{
  // User page is only one page, so we just erase it
  waitFSR();
  writeFCMD(CMD_EUP, 0);
}

bool
FlashCalWUserPage::isLocked()
{
  return false;
}

bool
FlashCalWUserPage::getLockRegion(uint32_t region)
{
  if (region >= 0)
    throw FlashRegionError();
  return false;
}

void
FlashCalWUserPage::setLockRegion(uint32_t region, bool enable)
{
  throw FlashRegionError();
}

uint32_t
FlashCalWUserPage::getWritePageCommand()
{
  return CMD_WUP;
}


uint32_t
FlashCalWUserPage::getErasePageCommand()
{
  return CMD_EUP;
}
