//
// @file NVMeFixPlugin.hpp
//
// NVMeFix
//
// Copyright © 2019 acidanthera. All rights reserved.
//
// This program and the accompanying materials
// are licensed and made available under the terms and conditions of the BSD License
// which accompanies this distribution.  The full text of the license may be found at
// http://opensource.org/licenses/bsd-license.php
// THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
// WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.


#ifndef NVMeFixPlugin_h
#define NVMeFixPlugin_h

#include <stdatomic.h>
#include <stdint.h>

#include <Library/LegacyIOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>

#include "nvme.h"
#include "nvme_quirks.hpp"

class NVMeFixPlugin {
public:
	friend class NVMePMProxy;

	void init();
	void deinit();
	static NVMeFixPlugin& globalPlugin();

	explicit NVMeFixPlugin() : PM(*this) {}
private:
	static void processKext(void*, KernelPatcher&, size_t, mach_vm_address_t, size_t);
	static bool matchingNotificationHandler(void*, void*, IOService*, IONotifier*);
	static bool terminatedNotificationHandler(void*, void*, IOService*, IONotifier*);
	bool solveSymbols(KernelPatcher& kp);

	atomic_bool solvedSymbols;

	IONotifier* matchingNotifier {nullptr}, * terminationNotifier {nullptr};

	/* Used for synchronising concurrent access to this class from notification handlers */
	IOLock* lck {nullptr};

	const char* kextPath {
		"/System/Library/Extensions/IONVMeFamily.kext/Contents/MacOS/IONVMeFamily"
	};

	KernelPatcher::KextInfo kextInfo {
		"com.apple.iokit.IONVMeFamily",
		&kextPath,
		1,
		{true},
		{},
		KernelPatcher::KextInfo::Unloaded
	};

	struct {
		template <typename T, typename... Args>
		struct Func {
			const char* const name;
			mach_vm_address_t fptr {};

			bool solve(KernelPatcher& kp, size_t idx) {
				/* Cache the result */
				if (!fptr)
					fptr = kp.solveSymbol(idx, name);
				return fptr;
			}

			bool route(KernelPatcher& kp, size_t idx, T(*repl)(Args...)) {
				if (!solve(kp, idx))
					return false;
				fptr = kp.routeFunction(fptr, reinterpret_cast<mach_vm_address_t>(repl), true);
				return fptr;
			}

			bool routeVirtual(KernelPatcher& kp, size_t idx, const char* vtFor, size_t offs, T(*repl)(Args...)) {
				using PtrTy = T(**)(Args...);
				assert(vtFor);
				auto vt = kp.solveSymbol(idx, vtFor);
				return vt && KernelPatcher::routeVirtual(&vt, offs, repl, reinterpret_cast<PtrTy>(&fptr));
			}

			T operator()(Args... args) const {
				assertf(fptr, "%s not solved", name ? name : "(unknown)");
				return (*reinterpret_cast<T(*)(Args...)>(fptr))(static_cast<Args&&>(args)...);
			}
		};

		struct {
			Func<IOReturn,void*,IOMemoryDescriptor*,void*,uint64_t> IssueIdentifyCommand {
				"__ZN16IONVMeController20IssueIdentifyCommandEP18IOMemoryDescriptorP16AppleNVMeRequestj"
			};

			Func<IOReturn,void*,void*> ProcessSyncNVMeRequest {
				"__ZN16IONVMeController22ProcessSyncNVMeRequestEP16AppleNVMeRequest"
			};

			Func<void*,void*,uint64_t> GetRequest {
				"__ZN16IONVMeController10GetRequestEj"
			};
			Func<void,void*,void*> ReturnRequest {
				"__ZN16IONVMeController13ReturnRequestEP16AppleNVMeRequest"
			};
			Func<bool,void*,unsigned long, unsigned long> activityTickle {};
			Func<void,void*,void*,int> FilterInterruptRequest {
				"__ZN16IONVMeController22FilterInterruptRequestEP28IOFilterInterruptEventSource"
			};
		} IONVMeController;

		struct {
			Func<void,void*,uint8_t> BuildCommandGetFeatures {
				"__ZN16AppleNVMeRequest23BuildCommandGetFeaturesEh"
			};

			Func<void,void*,uint8_t> BuildCommandSetFeaturesCommon {
				"__ZN16AppleNVMeRequest29BuildCommandSetFeaturesCommonEh"
			};

			Func<uint32_t,void*> GetStatus {
				"__ZN16AppleNVMeRequest9GetStatusEv"
			};

			Func<uint32_t,void*> GetOpcode {
				"__ZN16AppleNVMeRequest9GetOpcodeEv"
			};

			Func<IOReturn,void*,uint64_t,uint64_t> GenerateIOVMSegments {
				"__ZN16AppleNVMeRequest20GenerateIOVMSegmentsEyy"
			};
		} AppleNVMeRequest;
	} kextFuncs;


	/**
	 * Controller writes operation result to a member of AppleNVMeRequest which
	 * is then directly accessed by client code. Unlike the other request members,
	 * there is no wrapper, but it always seem to follow uint32_t status member in request
	 * struct, so we rely on that.
	 */
	struct {
		template <typename T>
		struct Member {
			mach_vm_address_t offs {};
			T& get(void* obj) {
				assert(offs);
				assert(obj);

				return getMember<T>(obj, offs);
			}

			bool fromFunc(mach_vm_address_t start, uint32_t opcode, uint32_t reg, uint32_t rm,
						  uint32_t add=0, size_t ninsts_max=128) {
				if (offs)
					return true;

				if (!start)
					return false;
				hde64s dis;

				for (size_t i = 0; i < ninsts_max; i++) {
					auto sz = Disassembler::hdeDisasm(start, &dis);

					if (dis.flags & F_ERROR)
						break;

					/* mov reg, [reg+disp] */
					if (dis.opcode == opcode && dis.modrm_reg == reg && dis.modrm_rm == rm) {
						offs = dis.disp.disp32 + add;
						return true;
					}

					start += sz;
				}

				return false;
			};
		};

		struct {
			Member<uint8_t> ANS2MSIWorkaround;
		} IONVMeController;

		struct {
			Member<uint32_t> result;
			Member<void*> controller;
			Member<NVMe::nvme_command> command;
			Member<IOBufferMemoryDescriptor*> prpDescriptor;
		} AppleNVMeRequest;
	} kextMembers;

	/*
	 * How far should we traverse ioreg searching for parent NVMe controller
	 * Typical depth is 9 on real setups
	 */
	static constexpr int controllerSearchDepth {20};

	struct ControllerEntry {
		IOService* controller {nullptr};
		bool processed {false};
		NVMe::nvme_quirks quirks {NVMe::NVME_QUIRK_NONE};
		uint64_t ps_max_latency_us {100000};
		IOPMPowerState* powerStates {nullptr};
		size_t nstates {0};
		IOLock* lck {nullptr};
		IOService* pm {nullptr};
		IOBufferMemoryDescriptor* identify {nullptr};
		bool apste {false};

		bool apstAllowed() {
			return !(quirks & NVMe::nvme_quirks::NVME_QUIRK_NO_APST) && ps_max_latency_us > 0;
		}

		static void deleter(ControllerEntry* entry) {
			assert(entry);

			/* PM functions don't check for validity of entry or its members, so let's stop it early */
			if (entry->pm) {
				if (entry->controller)
					entry->controller->deRegisterInterestedDriver(entry->pm);
				entry->pm->PMstop();
				entry->pm->release();
			}
			if (entry->powerStates)
				delete[] entry->powerStates;
			if (entry->identify)
				entry->identify->release();
			if (entry->lck)
				IOLockFree(entry->lck);

			delete entry;
		}

		explicit ControllerEntry(IOService* c) : controller(c) {
			lck = IOLockAlloc();
			assert(lck);
		}
	};

	evector<ControllerEntry*, ControllerEntry::deleter> controllers;
	void handleControllers();
	void handleController(ControllerEntry&);
	IOReturn identify(ControllerEntry&,IOBufferMemoryDescriptor*&);
	bool enableAPST(ControllerEntry&, const NVMe::nvme_id_ctrl*);
	IOReturn configureAPST(ControllerEntry&,const NVMe::nvme_id_ctrl*);
	IOReturn APSTenabled(ControllerEntry&, bool&);
	IOReturn dumpAPST(ControllerEntry&, int npss);
	IOReturn NVMeFeatures(ControllerEntry&, unsigned fid, unsigned* dword11, IOBufferMemoryDescriptor* desc,
							 uint32_t* res, bool set);
	ControllerEntry* entryForController(IOService*) const;
	struct PM {
		bool init(ControllerEntry&,const NVMe::nvme_id_ctrl*);
		bool solveSymbols(KernelPatcher&);

		static constexpr unsigned idlePeriod {2}; /* seconds */

		static bool activityTickle(void*,unsigned long,unsigned long);

		explicit PM(NVMeFixPlugin& plugin) : plugin(plugin) {}
	private:
		NVMeFixPlugin& plugin;
	} PM;
};

class NVMePMProxy : public IOService {
	OSDeclareDefaultStructors(NVMePMProxy)
public:
	virtual IOReturn setPowerState(
		unsigned long powerStateOrdinal,
		IOService *   whatDevice ) override;

	virtual IOReturn powerStateDidChangeTo(
		IOPMPowerFlags  capabilities,
		unsigned long   stateNumber,
		IOService *     whatDevice ) override;

	NVMeFixPlugin::ControllerEntry* entry {nullptr};
};

#endif /* NVMeFixPlugin_h */
