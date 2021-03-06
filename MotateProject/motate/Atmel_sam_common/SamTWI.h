/*
 Atmel_sam_common/SamTWI.h - Library for the Motate system
 http://github.com/synthetos/motate/

 Copyright (c) 2018 Robert Giseburt

 This file is part of the Motate Library.

 This file ("the software") is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License, version 2 as published by the
 Free Software Foundation. You should have received a copy of the GNU General Public
 License, version 2 along with the software. If not, see <http://www.gnu.org/licenses/>.

 As a special exception, you may use this file as part of a software library without
 restriction. Specifically, if other files instantiate templates or use macros or
 inline functions from this file, or you compile this file and link it with  other
 files to produce an executable, this file does not by itself cause the resulting
 executable to be covered by the GNU General Public License. This exception does not
 however invalidate any other reasons why the executable file might be covered by the
 GNU General Public License.

 THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY
 WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 */

#ifndef SAMTWI_H_ONCE
#define SAMTWI_H_ONCE

#include "sam.h"

#include "MotatePins.h"
#include "SamCommon.h"
#include <type_traits>

#include "SamTWIInternal.h"
#include "SamTWIDMA.h"

//NOTE: Currently only supporting master mode!!

namespace Motate {

template <int8_t twiPeripheralNumber>
struct TWIHardware_ : public Motate::TWI_internal::TWIInfo<twiPeripheralNumber> {
    using this_type_t                      = TWIHardware_<twiPeripheralNumber>;
    using info                             = Motate::TWI_internal::TWIInfo<twiPeripheralNumber>;
    static constexpr auto twiPeripheralNum = twiPeripheralNumber;

    static_assert(info::exists, "Using an unsupported TWI peripheral for this processor.");

    using info::twi;
    using info::peripheralId;
    using info::IRQ;

    TWIInterruptHandler* externalTWIInterruptHandler_;

    std::function<void(Interrupt::Type)> TWIDMAInterruptHandler_;
    static this_type_t     *twiInterruptHandler_;

    DMA<TWI_tag, twiPeripheralNumber> dma{TWIDMAInterruptHandler_};

    TWIHardware_() : TWIDMAInterruptHandler_{[&](Interrupt::Type hint) { this->handleInterrupts(hint); }} {
        // twiInterruptHandlerJumper_ = [&]() { this->handleInterrupts(); };
        twiInterruptHandler_ = this;
        SamCommon::enablePeripheralClock(peripheralId);

        // read the status register (Microchip says so)
        this->getSR();

        // Softare reset of TWI module
        this->resetModule();

        this->disable();
    }

    void init() {
        dma.reset();

        this->setSpeed();  // use default fast I2C

        this->enable();

        // always have IRQs enabled for TWI, lowest priority
        setInterrupts(TWIInterrupt::PriorityLow);
    }

    void handleInterrupts(Interrupt::Type hint = 0) {
        auto interruptCause = getInterruptCause(hint);
        prehandleInterrupt(interruptCause);
    }

    uint32_t internal_address_value_ = 0;
    uint8_t  internal_address_to_send_;

    bool setAddress(const TWIAddress& address, const TWIInternalAddress& internal_address) {
        // check for transmitting?

        uint8_t  adjusted_address               = 0;
        uint32_t adjusted_internal_address      = 0;
        uint8_t  adjusted_internal_address_size = 0;

        if (TWIDeviceAddressSize::k10Bit == address.size) {
            if (internal_address.size > TWIInternalAddressSize::k2Bytes) {
                return false;  // we only support 3 total bytes of internal address
            }
            // top two bits (xx) of the 10-bit address ORd with special code 0b011110xx go into the device address
            adjusted_address               = 0b01111000 | ((address.address >> 8) & 0b11);
            adjusted_internal_address      = (address.address & 0xFF) | ((internal_address.address & 0xFFFF) << 8);
            adjusted_internal_address_size = (uint8_t)internal_address.size + 1;
        } else {
            // 7 bit address
            adjusted_address               = 0x7F & address.address;
            adjusted_internal_address      = (internal_address.address & 0xFFFFFF);
            adjusted_internal_address_size = (uint8_t)internal_address.size;
        }

        this->_setAddress(adjusted_address, adjusted_internal_address, adjusted_internal_address_size);

        internal_address_value_   = 0;
        internal_address_to_send_ = 0;

        this->enable();
        return true;
    }

    void setInterruptHandler(TWIInterruptHandler *handler) {
        externalTWIInterruptHandler_ = handler;
    }

    TWIInterruptCause getInterruptCause(Interrupt::Type hint = 0) {
        TWIInterruptCause status;

        // Notes from experience:
        // This processor will sometimes allow one of these bits to be set,
        // even when there is no interrupt requested, and the setup conditions
        // don't appear to be done.
        // The simple but unfortunate fix is to verify that the Interrupt Mask
        // calls for that interrupt before considering it as a possible interrupt
        // source. This should be a best practice anyway, really. -Giseburt

        // If there's a hint, then this is coming from the DMA
        // Since this is a different interrupt handler for the DMA and the TWI,
        // if there is a hint, ONLY return the values for the DMA

        if (hint) {
            if ((hint & Interrupt::OnTxTransferDone)) {
                status.setTxTransferDone();
            }
            if ((hint & Interrupt::OnRxTransferDone)) {
                status.setRxTransferDone();
            }
            if ((hint & Interrupt::OnTxError)) {
                status.setTxError();
            }
            if ((hint & Interrupt::OnRxError)) {
                status.setRxError();
            }

            return status;
        }

        auto TWI_SR_hold = this->getSR();

        if (this->isTxReady(TWI_SR_hold)) {
            status.setTxReady();
        }
        if (this->isTxComp(TWI_SR_hold)) {
            status.setTxDone();
        }
        if (this->isRxReady(TWI_SR_hold)) {
            status.setRxReady();
        }
        if (this->isNack(TWI_SR_hold)) {
            status.setNACK();
        }

        return status;
    }

    void setInterrupts(const Interrupt::Type interrupts) {
        if (interrupts != TWIInterrupt::Off) {
            if (interrupts & TWIInterrupt::OnTxReady) {
                this->enableOnTXReadyInterrupt();
            } else {
                this->disableOnTXReadyInterrupt();
            }
            if (interrupts & TWIInterrupt::OnTxDone) {
                this->enableOnTXDoneInterrupt();
            } else {
                this->disableOnTXDoneInterrupt();
            }

            if (interrupts & TWIInterrupt::OnNACK) {
                this->enableOnNACKInterrupt();
            } else {
                this->disableOnNACKInterrupt();
            }

            if (interrupts & TWIInterrupt::OnRxReady) {
                this->enableOnRXReadyInterrupt();
            } else {
                this->disableOnRXReadyInterrupt();
            }

            if (interrupts & TWIInterrupt::OnRxTransferDone) {
                dma.startRxDoneInterrupts();
            } else {
                dma.stopRxDoneInterrupts();
            }
            if (interrupts & TWIInterrupt::OnTxTransferDone) {
                dma.startTxDoneInterrupts();
            } else {
                dma.stopTxDoneInterrupts();
            }

            /* Set interrupt priority */
            if (interrupts & TWIInterrupt::PriorityHighest) {
                NVIC_SetPriority(IRQ, 0);
            } else if (interrupts & TWIInterrupt::PriorityHigh) {
                NVIC_SetPriority(IRQ, 1);
            } else if (interrupts & TWIInterrupt::PriorityMedium) {
                NVIC_SetPriority(IRQ, 2);
            } else if (interrupts & TWIInterrupt::PriorityLow) {
                NVIC_SetPriority(IRQ, 3);
            } else if (interrupts & TWIInterrupt::PriorityLowest) {
                NVIC_SetPriority(IRQ, 4);
            }

            // Always have IRQs enabled for TWI
            NVIC_EnableIRQ(IRQ);
        } else {
            // Always have IRQs enabled for TWI
            // NVIC_DisableIRQ(IRQ);
        }
    }

    // void this->enableOnTXTransferDoneInterrupt_() {
    //     dma.startTxDoneInterrupts();
    // }

    // void this->disableOnTXTransferDoneInterrupt_() {
    //     dma.stopTxDoneInterrupts();
    // }

    // void this->enableOnRXTransferDoneInterrupt_() {
    //     dma.startRxDoneInterrupts();
    // }

    // void this->disableOnRXTransferDoneInterrupt_() {
    //     dma.stopRxDoneInterrupts();
    // }

    uint8_t getMessageSlotsAvailable() {
        uint8_t count = 0;
        if (dma.doneWriting() && dma.doneReading()) {
            count++;
        }
        // if (dma.doneWritingNext()) { count++; }
        return count;
    }

    bool doneWriting() { return dma.doneWriting(); }
    bool doneReading() { return dma.doneReading(); }

    enum class InternalState {
        Idle,

        TXReadyToSendFirstByte,
        TXSendingFirstByte,
        TXDMAStarted,
        TXWaitingForTXReady1,
        TXWaitingForTXReady2,
        TXError,
        TXDone,

        RXReadyToReadFirstByte,
        RXReadingFirstByte,
        RXDMAStarted,
        RXWaitingForRXReady,
        RXWaitingForLastChar,
        RXError,
        RXDone,
    };

    std::atomic<InternalState> state_ = InternalState::Idle;

    uint8_t* local_buffer_ptr_  = nullptr;
    uint16_t local_buffer_size_ = 0;

    // start transfer of message
    bool startTransfer(uint8_t* buffer, const uint16_t size, const bool is_rx) {
        if ((buffer == nullptr) || (state_ != InternalState::Idle) || (size == 0)) {
#if IN_DEBUGGER == 1
            __asm__("BKPT");
#endif
            return false;
        }
        local_buffer_ptr_ = nullptr;

        dma.setInterrupts(Interrupt::Off);
        if (is_rx) {
            local_buffer_ptr_  = buffer;
            local_buffer_size_ = size;

            // tell the peripheral that we're reading
            // (Note that internall address mode MIGHT actually write first!)
            this->setReading();

            if (local_buffer_size_ == 1) {
                // If this is the only character to read, tell it to NACK at the end of this read
                // and tart the reading transaction
                this->setStartStop();

                // "last char" is the only char
                state_ = InternalState::RXWaitingForLastChar;

            } else if (local_buffer_size_ > 2) {
                // Start the reading transaction
                this->setStart();

                state_ = InternalState::RXReadingFirstByte;

            } else {  // local_buffer_size_ == 2
                // Start the reading transaction
                this->setStart();

                state_ = InternalState::RXWaitingForRXReady;  // this will pick up below
                this->enableOnRXReadyInterrupt();
                this->enableOnNACKInterrupt();
                // return true;
            }
            this->enableOnNACKInterrupt();

            TWIInterruptCause empty_cause;
            prehandleInterrupt(empty_cause);  // ignore return value
        } else {
            // TX
            state_ = InternalState::TXReadyToSendFirstByte;

            local_buffer_ptr_  = buffer;
            local_buffer_size_ = size;

            this->enableOnTXReadyInterrupt();
            this->enableOnNACKInterrupt();
        }

        // enable();

        return true;
    }


    bool prehandleInterrupt(TWIInterruptCause& cause) {
        // return true if we are done with the transaction
        if (cause.isRxError()) {
            state_ = InternalState::RXError;
        }

        if (cause.isTxError()) {
            state_ = InternalState::TXError;
        }

        // NACK cases
        if (cause.isNACK() || state_ == InternalState::RXError || state_ == InternalState::TXError) {
            // __asm__("BKPT");
            this->disableOnRXReadyInterrupt();

            this->disableOnTXReadyInterrupt();
            this->disableOnTXDoneInterrupt();

            this->disableOnNACKInterrupt();

            // Problem: How to tell how much, if anything, was transmitted?

            dma.stopRxDoneInterrupts();
            dma.stopTxDoneInterrupts();

            return true;
        }

        // Spurious interrupt
        if (InternalState::Idle == state_) {
            // #if IN_DEBUGGER == 1
            // __asm__("BKPT");
            // #endif
            this->disableOnRXReadyInterrupt();

            this->disableOnTXReadyInterrupt();
            this->disableOnTXDoneInterrupt();

            dma.stopRxDoneInterrupts();
            dma.stopTxDoneInterrupts();

            cause.clear();
            return false;
        }

        // RX cases
        if (InternalState::RXReadingFirstByte == state_ && cause.isRxReady()) {
            // NOTE: local_buffer_size_ > 2 for RXReadingFirstByte state to be set
            cause.clearRxReady();  // stop this from being propagated

            this->disableOnRXReadyInterrupt();

            // From SAM E70/S70/V70/V71 Family Eratta:
            //  If TCM accesses are generated through the AHBS port of the core, only 32-bit accesses are supported.
            //  Accesses that are not 32-bit aligned may overwrite bytes at the beginning and at the end of 32-bit
            //  words.
            // Workaround
            //  The user application must use 32-bit aligned buffers and buffers with a size of a multiple of 4 bytes
            //  when transferring data to or from the TCM through the AHBS port of the core.

            // Nothing to be done about the scribble of data at the end, other than oversize the buffers accordingly.

            // But to handle the beginning alignment, read unaligned bytes manally

            if ((std::intptr_t)local_buffer_ptr_ & 0b11) {
                *local_buffer_ptr_ = this->readByte();
                ++local_buffer_ptr_;
                --local_buffer_size_;

                // now leave, and at the next RxReady, we'll be back here
                return false;
            }

            // The first byte to read is in the RHR register, and DMA will grab it first
            const bool handle_interrupts = true;
            const bool include_next      = false;

            state_ = InternalState::RXDMAStarted;

            // Note we set size to size-2 since we have to handle the last two characters "manually"
            // This happens in prehandleInterrupt()
            bool dma_is_setup =
                dma.startRXTransfer(local_buffer_ptr_, local_buffer_size_ - 2, handle_interrupts, include_next);
            if (!dma_is_setup) {
                state_ = InternalState::RXError;
                cause.setRxError();
                return true;
            }  // fail early

            local_buffer_ptr_ = local_buffer_ptr_ + (local_buffer_size_ - 2);
        }  // no else here!

        if (InternalState::RXDMAStarted == state_ && cause.isRxTransferDone()) {
            cause.clearRxTransferDone();  // stop this from being propagated
            // At this point we've recieved all but the last two characters
            // Now we wait for the next-to-last character to come in, read it into the buffer, and set the STOP bit
            state_ = InternalState::RXWaitingForRXReady;

            // IMPORTANT: This code is called from the XDMAC interrupt, NOT the TWIHS interrupt!
            // It's possible to accidentally trifgger the TWIHS interrupt and have both covering this state.

            dma.stopRxDoneInterrupts();
            dma.disable();

            this->enableOnRXReadyInterrupt();
            return false;
        }  // no else here!

        if (InternalState::RXWaitingForRXReady == state_) {
            if (!this->isRxReady()) {
                this->enableOnRXReadyInterrupt();
                return false;
            }
            // while (!this->isRxReady()) {
            //     ;
            // }
            cause.clearRxReady();  // stop this from being propagated

            // At this point we've recieved the next-to-last character, but haven't read it yet

            // Now we set the STOP bit, ...
            this->setStop();

            /// ... read the next-to-last char .. .
            *local_buffer_ptr_ = this->readByte();
            ++local_buffer_ptr_;

            // ... prepare to wait for the last character
            state_ = InternalState::RXWaitingForLastChar;
        }  // no else here!

        if (InternalState::RXWaitingForLastChar == state_) {
            // if (!this->isRxReady()) {
            //     this->enableOnRXReadyInterrupt();
            //     return false;
            // }
            while (!this->isRxReady()) { ; }

            cause.clearRxReady();       // stop this from being propagated
            cause.setRxTransferDone();  // And now we push that the rx transfer is done

            // At this point we've recieved last character, but haven't read it yet
            *local_buffer_ptr_ = this->readByte();
            local_buffer_ptr_  = nullptr;

            // Finsih the state machine, and stop the interrupts
            state_ = InternalState::RXDone;
            this->disableOnRXReadyInterrupt();
            this->disableOnNACKInterrupt();
        }  // no else here!


        // TX cases
        if (InternalState::TXReadyToSendFirstByte == state_ && cause.isTxReady()) {
            // tell the peripheral that we're writing
            this->setWriting();

            if (local_buffer_size_ > 2) {
                cause.clearTxReady();  // stop this from being propagated

                state_         = InternalState::TXSendingFirstByte;
                this->transmitChar(*local_buffer_ptr_);
                ++local_buffer_ptr_;
                --local_buffer_size_;

                return false;  // nothing more to do, leave now
            } else {
                state_ = InternalState::TXWaitingForTXReady1;  // this will pick up below
            }
        }  // no else here!

        if (InternalState::TXSendingFirstByte == state_ && cause.isTxReady()) {
            if (local_buffer_size_ > 1) {
                cause.clearTxReady();  // stop this from being propagated
                this->disableOnTXReadyInterrupt();
                const bool handle_interrupts = true;
                const bool include_next      = false;

                // this->enableOnTXDoneInterrupt();
                this->enableOnNACKInterrupt();

                // Note we set size to size-1 since we have to handle the last character "manually"
                // This happens in prehandleInterrupt()
                bool dma_is_setup =
                    dma.startTXTransfer(local_buffer_ptr_, local_buffer_size_ - 1, handle_interrupts, include_next);
                if (!dma_is_setup) {
                    state_ = InternalState::TXError;
                    cause.setTxError();
                    return true;
                }  // fail early

                local_buffer_ptr_  = local_buffer_ptr_ + (local_buffer_size_ - 1);
                local_buffer_size_ = 1;
                state_             = InternalState::TXDMAStarted;
            } else {
                state_ = InternalState::TXWaitingForTXReady1;
                this->enableOnTXReadyInterrupt();
            }
        }  // no else here!

        if (InternalState::TXDMAStarted == state_ && (cause.isTxTransferDone())) {
            // cause.clearTxDone(); // stop this from being propagated
            cause.clearTxTransferDone();  // stop this from being propagated
            // At this point DMA has sent all but the last character to the TWI
            // Now we wait for the TWI hardware to indicate it's ready for another character
            // which may have already happened
            state_ = InternalState::TXWaitingForTXReady1;
            dma.stopTxDoneInterrupts();
            dma.disable();

            // If we don't already have an TXReady, set the interrupt for it
            // if (!isTxReady()) {
            this->enableOnTXReadyInterrupt();
            // return;
            // }
        }  // no else here!

        if (InternalState::TXWaitingForTXReady1 == state_ && (cause.isTxDone() || cause.isTxTransferDone())) {
            cause.clearTxDone();          // stop this from being propagated
            cause.clearTxTransferDone();  // stop this from being propagated
        }                                 // no else here!

        if (InternalState::TXWaitingForTXReady1 == state_ && (this->isTxReady() || cause.isTxReady())) {
            cause.clearTxReady();  // stop this from being propagated
            this->disableOnTXReadyInterrupt();

            // At this point we've sent the next-to-last character,
            // ... set the STOP bit, ...
            this->setStop();

            // ... and put the last char in the hold register, ...
            this->transmitChar(*local_buffer_ptr_);
            local_buffer_ptr_ = nullptr;

            // ... and move the state machine along.
            state_ = InternalState::TXWaitingForTXReady2;
            this->enableOnTXDoneInterrupt();

            // return; // there's nothing more we can do here, save time
        }  // no else here!

        if (InternalState::TXWaitingForTXReady2 == state_ && (cause.isTxReady() || cause.isTxDone())) {
            // we want isTxReady to be propagated
            cause.setTxTransferDone();  // And now we push that the tx transfer is done

            // Finish up the state machine.
            state_ = InternalState::TXDone;
            this->disableOnTXReadyInterrupt();
            this->disableOnTXDoneInterrupt();
            this->disableOnNACKInterrupt();
        }

        if (state_ == InternalState::TXDone || state_ == InternalState::RXDone) {
            state_ = InternalState::Idle;

            if (externalTWIInterruptHandler_) {
                externalTWIInterruptHandler_->handleTWIInterrupt(cause);
            } else {
#if IN_DEBUGGER == 1
                __asm__("BKPT");
#endif
            }

            return true;
        }

        return false;
    }  // prehandleInterrupt

    // abort transfer of message
    // TODO

    // get transfer status
    // TODO
    };

    // TWIGetHardware is just a pass-through for now
    template <pin_number twiSCKPinNumber, pin_number twiSDAPinNumber>
    using TWIGetHardware = TWIHardware_<TWISCKPin<twiSCKPinNumber>::twiNum>;

} // namespace Motate

#endif /* end of include guard: SAMTWI_H_ONCE */
