//
//  riscv-device-config.h
//

#ifndef riscv_device_config_h
#define riscv_device_config_h

namespace riscv {

	/* Config MMIO device */

	template <typename P, const int NUM_IRQS = 64, const int NUM_NODES = 1, const int NUM_HARTS = 8>
	struct config_mmio_device : memory_segment<typename P::ux>
	{
		typedef typename P::ux UX;

		enum {
			total_size = sizeof(UX) * 4
		};

		P &proc;

		/* Config registers */

		UX num_harts;
		UX time_base;
		UX rom_base;
		UX rom_entry;

		/* Config data access */

		constexpr u8* as_u8() { return (u8*)&num_harts; }
		constexpr u16* as_u16() { return (u16*)&num_harts; }
		constexpr u32* as_u32() { return (u32*)&num_harts; }
		constexpr u64* as_u64() { return (u64*)&num_harts; }

		/* Config constructor */

		config_mmio_device(P &proc, UX mpa) :
			memory_segment<UX>("CFG0", mpa, /*uva*/0, /*size*/total_size,
				pma_type_io | pma_prot_read), proc(proc)
			{}

		/* Config interface */

		void print_registers()
		{
			debug("cfg_mmio :num_harts        %d", num_harts);
			debug("cfg_mmio :time_base        %d", time_base);
			debug("cfg_mmio :rom_base         0x%llx", rom_base);
			debug("cfg_mmio :rom_entry        0x%llx", rom_entry);
		}

		/* Config MMIO */

		void load_8 (UX va, u8  &val)
		{
			val = (va < total_size) ? *(as_u8() + va) : 0;
			if (proc.log & proc_log_mmio) {
				printf("cfg_mmio :0x%04llx -> 0x%02hhx\n", addr_t(va), val);
			}
		}

		void load_16(UX va, u16 &val)
		{
			val = (va < total_size - 1) ? *(as_u16() + (va>>1)) : 0;
			if (proc.log & proc_log_mmio) {
				printf("cfg_mmio :0x%04llx -> 0x%04hx\n", addr_t(va), val);
			}
		}

		void load_32(UX va, u32 &val)
		{
			val = (va < total_size - 3) ? *(as_u32() + (va>>2)) : 0;
			if (proc.log & proc_log_mmio) {
				printf("cfg_mmio :0x%04llx -> 0x%08x\n", addr_t(va), val);
			}
		}

		void load_64(UX va, u64 &val)
		{
			val = (va < total_size - 7) ? *(as_u64() + (va>>3)) : 0;
			if (proc.log & proc_log_mmio) {
				printf("cfg_mmio :0x%04llx -> 0x%016llx\n", addr_t(va), val);
			}
		}
	};

}

#endif