//
//  rv-asm.cc
//

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cinttypes>
#include <cstdarg>
#include <cerrno>
#include <cassert>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <set>

#include <unistd.h>

#include "host-endian.h"
#include "types.h"
#include "bits.h"
#include "format.h"
#include "meta.h"
#include "util.h"
#include "cmdline.h"
#include "color.h"
#include "codec.h"
#include "strings.h"
#include "disasm.h"
#include "assembler.h"
#include "elf.h"
#include "elf-file.h"
#include "elf-format.h"
#include "shunting-yard.h"

#define DEBUG 1

using namespace riscv;
using namespace std::placeholders;

struct asm_filename;
struct asm_line;
struct asm_macro;

typedef std::shared_ptr<asm_filename> asm_filename_ptr;
typedef std::shared_ptr<asm_line> asm_line_ptr;
typedef std::shared_ptr<asm_macro> asm_macro_ptr;
typedef std::function<bool(asm_line_ptr&)>asm_directive;

struct asm_filename
{
	std::string filename;

	asm_filename(std::string filename) :
		filename(filename) {}
};

struct asm_line
{
	asm_filename_ptr file;
	int line_num;
	std::vector<std::string> args;

	asm_line(asm_filename_ptr file, int line_num, std::vector<std::string> args) :
		file(file), line_num(line_num), args(args) {}

	std::string ref()
	{
		return file->filename + ":" + std::to_string(line_num);
	}

	std::deque<std::vector<std::string>> split_args(std::string sep)
	{
		std::deque<std::vector<std::string>> vec;
		vec.push_back(std::vector<std::string>());
		for (auto i = args.begin() + 1; i != args.end(); i++) {
			if (*i == sep) {
				vec.push_back(std::vector<std::string>());
			} else {
				vec.back().push_back(*i);
			}
		}
		return vec;
	}
};

struct asm_macro
{
	asm_line_ptr macro_def;
	std::vector<asm_line_ptr> macro_lines;

	asm_macro(asm_line_ptr macro_def) :
		macro_def(macro_def) {}
};

struct rv_assembler
{
	std::string input_filename;
	std::string output_filename = "a.out";
	bool help_or_error = false;
	bool bail_on_errors = true;
	bool debug = false;

	int ext = rv_set_imafdc;
	int width = rv_isa_rv64;

	assembler as;
	TokenMap vars;
	asm_macro_ptr defining_macro;
	std::vector<asm_macro_ptr> macro_stack;
	std::map<std::string,size_t> ireg_map;
	std::map<std::string,size_t> freg_map;
	std::map<std::string,size_t> csr_map;
	std::map<std::string,size_t> opcode_map;
	std::map<std::string,asm_macro_ptr> macro_map;
	std::map<std::string,asm_directive> directive_map;

	rv_assembler()
	{
		configure_maps();
		configure_directives();
	}

	void populate_map(std::map<std::string,size_t> &map, const char** arr)
	{
		size_t i = 0;
		while(arr[i] != nullptr) {
			map[arr[i]] = i;
			i++;
		}
	}

	void populate_csr_map(std::map<std::string,size_t> &map, const char** arr)
	{
		for (int i = 0; i < 4096; i++) {
			if (arr[i] == nullptr) continue;
			map[arr[i]] = i;
		}
	}

	void configure_maps()
	{
		for (size_t i = 0; i < 32; i++) {
			std::string ireg = "x" + std::to_string(i);
			std::string freg = "f" + std::to_string(i);
			ireg_map[ireg] = i;
			freg_map[freg] = i;
		}
		populate_map(ireg_map, rv_ireg_name_sym);
		populate_map(freg_map, rv_freg_name_sym);
		populate_map(opcode_map, rv_inst_name_sym);
		populate_csr_map(csr_map, rv_csr_name_sym);
	}

	void configure_directives()
	{
		directive_map[".align"] = std::bind(&rv_assembler::handle_p2align, this, _1);
		directive_map[".p2align"] = std::bind(&rv_assembler::handle_p2align, this, _1);
		directive_map[".balign"] = std::bind(&rv_assembler::handle_balign, this, _1);
		directive_map[".equ"] = std::bind(&rv_assembler::handle_equ, this, _1);
		directive_map[".eqv"] = std::bind(&rv_assembler::handle_equ, this, _1);
		directive_map[".file"] = std::bind(&rv_assembler::handle_file, this, _1);
		directive_map[".globl"] = std::bind(&rv_assembler::handle_globl, this, _1);
		directive_map[".ident"] = std::bind(&rv_assembler::handle_ident, this, _1);
		directive_map[".macro"] = std::bind(&rv_assembler::handle_macro, this, _1);
		directive_map[".endm"] = std::bind(&rv_assembler::handle_endm, this, _1);
		directive_map[".section"] = std::bind(&rv_assembler::handle_section, this, _1);
		directive_map[".text"] = std::bind(&rv_assembler::handle_text, this, _1);
		directive_map[".data"] = std::bind(&rv_assembler::handle_data, this, _1);
		directive_map[".rodata"] = std::bind(&rv_assembler::handle_rodata, this, _1);
		directive_map[".bss"] = std::bind(&rv_assembler::handle_bss, this, _1);
		directive_map[".size"] = std::bind(&rv_assembler::handle_size, this, _1);
		directive_map[".string"] = std::bind(&rv_assembler::handle_string, this, _1);
		directive_map[".type"] = std::bind(&rv_assembler::handle_type, this, _1);
		directive_map[".byte"] = std::bind(&rv_assembler::handle_byte, this, _1);
		directive_map[".half"] = std::bind(&rv_assembler::handle_half, this, _1);
		directive_map[".word"] = std::bind(&rv_assembler::handle_word, this, _1);
		directive_map[".dword"] = std::bind(&rv_assembler::handle_dword, this, _1);
		directive_map[".dtprelword"] = std::bind(&rv_assembler::handle_dtprelword, this, _1);
		directive_map[".dtpreldword"] = std::bind(&rv_assembler::handle_dtpreldword, this, _1);
		directive_map[".option"] = std::bind(&rv_assembler::handle_option, this, _1);
		directive_map[".zero"] = std::bind(&rv_assembler::handle_zero, this, _1);
		directive_map["la"] = std::bind(&rv_assembler::handle_la, this, _1);
		directive_map["lla"] = std::bind(&rv_assembler::handle_lla, this, _1);
		directive_map["li"] = std::bind(&rv_assembler::handle_li, this, _1);
		directive_map["call"] = std::bind(&rv_assembler::handle_call, this, _1);
		directive_map["tail"] = std::bind(&rv_assembler::handle_tail, this, _1);
	}

	static rv_set decode_isa_ext(std::string isa_ext)
	{
		if (strncasecmp(isa_ext.c_str(), "IMA", isa_ext.size()) == 0) return rv_set_ima;
		else if (strncasecmp(isa_ext.c_str(), "IMAC", isa_ext.size()) == 0) return rv_set_imac;
		else if (strncasecmp(isa_ext.c_str(), "IMAFD", isa_ext.size()) == 0) return rv_set_imafd;
		else if (strncasecmp(isa_ext.c_str(), "IMAFDC", isa_ext.size()) == 0) return rv_set_imafdc;
		else return rv_set_none;
	}

	template <typename T>
	std::string join(std::vector<T> list, std::string sep)
	{
		std::stringstream ss;
		for (auto i = list.begin(); i != list.end(); i++) {
			ss << (i != list.begin() ? sep : "") << *i;
		}
		return ss.str();
	}

	void parse_commandline(int argc, const char *argv[])
	{
		cmdline_option options[] =
		{
			{ "-h", "--help", cmdline_arg_type_none,
				"Show help",
				[&](std::string s) { return (help_or_error = true); } },
			{ "-d", "--debug", cmdline_arg_type_none,
				"Show help",
				[&](std::string s) { return (debug = true); } },
			{ "-i", "--isa", cmdline_arg_type_string,
				"ISA Extensions (IMA, IMAC, IMAFD, IMAFDC)",
				[&](std::string s) { return (ext = decode_isa_ext(s)); } },
			{ "-m32", "--riscv32", cmdline_arg_type_string,
				"Assembler for RISC-V 32",
				[&](std::string s) { return (width = rv_isa_rv32); } },
			{ "-m64", "--riscv64", cmdline_arg_type_string,
				"Assembler for RISC-V 64 (default)",
				[&](std::string s) { return (width = rv_isa_rv64); } },
			{ "-o", "--output", cmdline_arg_type_string,
				"Output ELF file (default a.out)",
				[&](std::string s) { output_filename = s; return true; } },
			{ nullptr, nullptr, cmdline_arg_type_none,   nullptr, nullptr }
		};

		auto result = cmdline_option::process_options(options, argc, argv);
		if (!result.second) {
			help_or_error = true;
		} else if (result.first.size() != 1 && !help_or_error) {
			printf("%s: wrong number of arguments\n", argv[0]);
			help_or_error = true;
		}

		if (help_or_error)
		{
			printf("usage: %s [<options>] <asm_file>\n", argv[0]);
			cmdline_option::print_options(options);
			exit(9);
		}

		input_filename = result.first[0];
	}

	std::vector<std::string> parse_line(std::string line)
	{
		// simple parsing routine that handles tokens separated by whitespace
		// separator characters, double quoted tokens containing and # comments

		std::string specials = "%:,+-*/()";
		std::vector<char> token;
		std::vector<std::string> args;
		enum {
			whitespace,
			quoted_token,
			unquoted_token,
			comment
		} state = whitespace;

		size_t i = 0;
		while (i < line.size()) {
			char c = line[i];
			switch (state) {
				case whitespace: {
					if (::isspace(c)) {
						i++;
					} else if (c == '#') {
						state = comment;
					} else if (c == '"') {
						state = quoted_token;
						i++;
					} else {
						state = unquoted_token;
					}
					break;
				}
				case quoted_token: {
					if (c == '"') {
						args.push_back(std::string(token.begin(), token.end()));
						token.resize(0);
						state = whitespace;
					} else {
						token.push_back(c);
					}
					i++;
					break;
				}
				case unquoted_token: {
					auto s = specials.find(c);
					if (s != std::string::npos) {
						if (token.size() > 0) {
							args.push_back(std::string(token.begin(), token.end()));
						}
						args.push_back(specials.substr(s, 1));
						token.resize(0);
					} else if (::isspace(c)) {
						if (token.size() > 0) {
							args.push_back(std::string(token.begin(), token.end()));
						}
						token.resize(0);
						state = whitespace;
					} else {
						token.push_back(c);
					}
					i++;
					break;
				}
				case comment: {
					i++;
					break;
				}
			}
		}
		if (token.size() > 0) {
			args.push_back(std::string(token.begin(), token.end()));
		}
		return args;
	}

	void read_source(std::vector<asm_line_ptr> &data, std::string filename)
	{
		asm_filename_ptr file = std::make_shared<asm_filename>(filename);
		std::ifstream in(filename.c_str());
		std::string line;
		if (!in.is_open()) {
			panic("error opening %s\n", filename.c_str());
		}
		int line_num = 0;
		while (in.good())
		{
			line_num++;
			std::getline(in, line);
			size_t hoffset = line.find("#");
			if (hoffset != std::string::npos) {
				line = ltrim(rtrim(line.substr(0, hoffset)));
			}
			std::vector<std::string> args = parse_line(line);
			if (args.size() == 0) continue;
			if (args.size() == 2 && args[0] == ".include") {
				/* NOTE: we don't do any loop detection */
				read_source(data, args[1]);
			} else {
				data.push_back(std::make_shared<asm_line>(file, line_num, args));
			}
		}
		in.close();
	}

	/* handlers */

	bool handle_balign(asm_line_ptr &line)
	{
		if (line->args.size() < 2) {
			printf("%s invalid parameters\n", line->ref().c_str());
			return false;
		}
		s64 val;
		if (!parse_integral(line->args[1], val)) {
			printf("%s invalid number\n", line->ref().c_str());
			return false;
		}
		as.balign(val);
		return true;
	}

	bool handle_p2align(asm_line_ptr &line)
	{
		if (line->args.size() < 2) {
			printf("%s missing parameter\n", line->ref().c_str());
			return false;
		}
		s64 val;
		if (!parse_integral(line->args[1], val)) {
			printf("%s invalid number\n", line->ref().c_str());
			return false;
		}
		as.p2align(val);
		return true;
	}

	packToken eval(asm_line_ptr &line, std::vector<std::string> tokens)
	{
		/*
		 * TODO - handle % expansions
		 *
		 * %hi(symbol)               Absolute imm20
		 * %lo(symbol)               Absolute imm12
		 * %pcrel_hi(symbol)         PC-relative imm20
		 * %pcrel_lo(label)          PC-relative imm12
		 * %tls_ie_pcrel_hi(symbol)  TLS IE GOT "Initial Exec"
		 * %tls_gd_pcrel_hi(symbol)  TLS GD GOT "Global Dynamic"
		 * %tprel_hi(symbol)         TLS LE "Local Exec"
		 * %tprel_lo(label)          TLS LE "Local Exec"
		 * %tprel_add(x)             TLS LE "Local Exec"
		 * %gprel(symbol)            GP-relative
		 *
		 */
		if (tokens.size() == 1) {
			s64 val;
			if (parse_integral(tokens[0], val)) {
				return packToken(int64_t(val));
			}
		}
		if (tokens.size() > 1 && tokens[0] == "%") {
			printf("%s unimplemented function %%%s\n", line->ref().c_str(),
				tokens[1].c_str());
			return packToken(0);
		}
		std::string expr = join(tokens, " ");
		calculator calc(expr.c_str());
		auto result = calc.eval(vars);
		return result;
	}

	bool handle_equ(asm_line_ptr &line)
	{
		auto argv = line->split_args(",");
		if (argv.size() != 2 || argv[0].size() != 1 || argv[1].size() < 1) {
			printf("%s invalid parameters\n", line->ref().c_str());
			return false;
		}
		std::string var_name = argv[0][0];
		auto result = eval(line, argv[1]);
		vars[var_name] = result;
		return true;
	}

	bool handle_file(asm_line_ptr &line)
	{
		if (line->args.size() != 2) {
			printf("%s invalid parameters\n", line->ref().c_str());
			return false;
		}
		/* TODO */
		return true;
	}

	bool handle_globl(asm_line_ptr &line)
	{
		if (line->args.size() != 2) {
			printf("%s invalid parameters\n", line->ref().c_str());
			return false;
		}
		as.global(line->args[1]);
		return true;
	}

	bool handle_ident(asm_line_ptr &line)
	{
		if (line->args.size() != 2) {
			printf("%s invalid parameters\n", line->ref().c_str());
			return false;
		}
		/* ignore */
		return true;
	}

	bool handle_macro(asm_line_ptr &line)
	{
		if (line->args.size() < 2) {
			printf("%s invalid parameters\n", line->ref().c_str());
			return false;
		}
		if (defining_macro) {
			printf("%s already defining macro\n", line->ref().c_str());
			return false;
		}
		defining_macro = std::make_shared<asm_macro>(line);
		macro_map[line->args[1]] = defining_macro;
		return true;
	}

	bool handle_endm(asm_line_ptr &line)
	{
		if (line->args.size() != 1) {
			printf("%s invalid parameters\n", line->ref().c_str());
			return false;
		}
		defining_macro = asm_macro_ptr();
		return true;
	}

	bool handle_section(asm_line_ptr &line)
	{
		auto argv = line->split_args(",");
		if (argv.size() < 1 || argv[0].size() < 1) {
			printf("%s invalid parameters\n", line->ref().c_str());
			return false;
		}
		if (argv[0][0][0] != '.') {
			printf("%s section must begin with '.'\n", line->ref().c_str());
			return false;
		}
		as.get_section(argv[0][0]);
		return true;
	}

	bool handle_text(asm_line_ptr &line)
	{
		if (line->args.size() != 1) {
			printf("%s invalid parameters\n", line->ref().c_str());
			return false;
		}
		as.get_section(".text");
		return true;
	}

	bool handle_data(asm_line_ptr &line)
	{
		if (line->args.size() != 1) {
			printf("%s invalid parameters\n", line->ref().c_str());
			return false;
		}
		as.get_section(".data");
		return true;
	}

	bool handle_rodata(asm_line_ptr &line)
	{
		if (line->args.size() != 1) {
			printf("%s invalid parameters\n", line->ref().c_str());
			return false;
		}
		as.get_section(".rodata");
		return true;
	}

	bool handle_bss(asm_line_ptr &line)
	{
		if (line->args.size() != 1) {
			printf("%s invalid parameters\n", line->ref().c_str());
			return false;
		}
		as.get_section(".bss");
		return true;
	}

	bool handle_size(asm_line_ptr &line)
	{
		/* ignore */
		return true;
	}

	bool handle_string(asm_line_ptr &line)
	{
		if (line->args.size() != 2) {
			printf("%s invalid parameters\n", line->ref().c_str());
			return false;
		}
		std::string str =  line->args[1];
		for (size_t i = 0; i < str.size(); i++) {
			as.append(u8(str[i]));
		}
		as.append(u8(0));
		return true;
	}

	bool handle_type(asm_line_ptr &line)
	{
		/* ignore */
		return true;
	}

	template <typename T>
	bool handle_words(asm_line_ptr &line)
	{
		auto argv = line->split_args(",");
		if (argv.size() != 1 || argv[0].size() < 1) {
			printf("%s invalid parameters\n", line->ref().c_str());
			return false;
		}
		for (size_t i = 0; i < argv.size(); i++) {
			auto result = eval(line, argv[0]);
			if (T(result.asInt()) > std::numeric_limits<T>::max() ||
				T(result.asInt()) < std::numeric_limits<T>::min()) {
				printf("%s warning: value out of range\n", line->ref().c_str());
			}
			as.append(T(result.asInt()));
		}
		return true;
	}

	bool handle_byte(asm_line_ptr &line)
	{
		return handle_words<u8>(line);
	}

	bool handle_half(asm_line_ptr &line)
	{
		return handle_words<u16>(line);
	}

	bool handle_word(asm_line_ptr &line)
	{
		return handle_words<u32>(line);
	}

	bool handle_dword(asm_line_ptr &line)
	{
		return handle_words<u64>(line);
	}

	bool handle_dtprelword(asm_line_ptr &line)
	{
		/* TODO */
		return true;
	}

	bool handle_dtpreldword(asm_line_ptr &line)
	{
		/* TODO */
		return true;
	}

	bool handle_option(asm_line_ptr &line)
	{
		/* TODO - rvc,norvc,push,pop */
		return true;
	}

	bool handle_zero(asm_line_ptr &line)
	{
		auto argv = line->split_args(",");
		if (argv.size() != 1 || argv[0].size() != 1) {
			printf("%s invalid parameters\n", line->ref().c_str());
			return false;
		}
		/* TODO - handle expression */
		s64 val;
		if (!parse_integral(argv[0][0], val)) {
			printf("%s invalid number\n", line->ref().c_str());
			return false;
		}
		for (s64 i = 0; i < val; i++) {
			as.append(u8(0));
		}
		return true;
	}

	bool handle_la(asm_line_ptr &line)
	{
		/*
		 * TODO
		 *
		 * .1: auipc r,    %pcrel_hi(symbol)
		 *     addi  r, r, %pcrel_lo(1b)
		 *
		 * relocs: R_RISCV_PCREL_HI20, R_RISCV_PCREL_LO12_I
		 */
		return true;
	}

	bool handle_lla(asm_line_ptr &line)
	{
		/*
		 * TODO
		 *
		 * .1: auipc r,    %pcrel_hi(symbol)
		 *     addi  r, r, %pcrel_lo(1b)
		 *
		 * relocs: R_RISCV_PCREL_HI20, R_RISCV_PCREL_LO12_I
		 */
		return true;
	}

	bool handle_li(asm_line_ptr &line)
	{
		/*
		 * TODO
		 *
		 * lui, addiw, slli, addi
		 */
		auto argv = line->split_args(",");
		if (argv.size() != 2) {
			printf("%s invalid parameters\n", line->ref().c_str());
			return false;
		}

		/* register parameter */
		auto ri = argv[0].size() == 1 ? ireg_map.find(argv[0][0]) : ireg_map.end();
		if (ri == ireg_map.end()) {
			printf("%s invalid register %s\n", line->ref().c_str(),
				join(argv[0], " ").c_str());
			return false;
		}

		/* immediate parameter */
		auto result = eval(line, argv[1]);
		s64 imm = result.asInt();
		if (imm < -2048 || imm > 2047) {
			printf("%s unimplemented large immediate\n", line->ref().c_str());
			return false;
		}

		decode dec{};
        dec.op = rv_op_addi;
        dec.rd = ri->second;
        dec.rs1 = rv_ireg_zero;
        dec.imm = imm;

		as.append(u32(encode_inst(dec)));
		return true;
	}

	bool handle_call(asm_line_ptr &line)
	{
		/*
		 * TODO
		 *
		 * .1: auipc t1,     %pcrel_hi(symbol)
		 *     jalr  ra, t1, %pcrel_lo(1b)
		 *
		 * relocs: R_RISCV_CALL_PLT
		 */
		return true;
	}

	bool handle_tail(asm_line_ptr &line)
	{
		/*
		 * TODO
		 *
		 * .1: auipc t1,       %pcrel_hi(symbol)
		 *     jalr  zero, t1, %pcrel_lo(1b)
		 */
		return true;
	}

	asm_line_ptr macro_substitute(asm_line_ptr &line)
	{
		/* TODO - use macro_stack.back()->macro_def to substitute args */
		return line;
	}

	std::vector<rv_operand_data> opcode_operand_data(size_t op)
	{
		std::vector<rv_operand_data> op_data;
		const rv_operand_data *data = rv_inst_operand_data[op];
		while(data->type != rv_type_none) {
			op_data.push_back(*data++);
		}
		return op_data;
	}

	bool remove_operand(std::vector<rv_operand_data> &op_data, rv_type type)
	{
		for (auto oi = op_data.begin(); oi != op_data.end(); ) {
			if (oi->type == type) {
				oi = op_data.erase(oi);
				return true;
			} else {
				oi++;
			}
		}
		return false;
	}

	bool handle_opcode(size_t op, asm_line_ptr &line)
	{
		auto argv = line->split_args(",");

		decode dec{};
		dec.op = op;
		auto op_data = opcode_operand_data(op);
		const char *fmt = rv_inst_format[op];

		while (*fmt) {
			switch (*fmt) {
				case '(': break;
				case ',': break;
				case ')': break;
				case '0':
				case '1':
				case '2':
				{
					if (argv.size() == 0) {
						printf("%s missing register parameter\n", line->ref().c_str());
						return false;
					}
					auto arg = argv.front();
					auto ri = arg.size() == 1 ? ireg_map.find(arg[0]) : ireg_map.end();
					if (ri == ireg_map.end()) {
						printf("%s invalid register %s\n", line->ref().c_str(),
							join(argv[0], " ").c_str());
						return false;
					}
					switch (*fmt) {
						case '0': dec.rd = ri->second; break;
						case '1': dec.rs1 = ri->second; break;
						case '2': dec.rs2 = ri->second; break;
					}
					remove_operand(op_data, rv_type_ireg);
					argv.pop_front();
					break;
				}
				case '3':
				case '4':
				case '5':
				case '6':
				{
					if (argv.size() == 0) {
						printf("%s missing register parameter\n", line->ref().c_str());
						return false;
					}
					auto arg = argv.front();
					auto ri = arg.size() == 1 ? ireg_map.find(arg[0]) : ireg_map.end();
					if (ri == freg_map.end()) {
						printf("%s invalid register %s\n", line->ref().c_str(),
							join(argv[0], " ").c_str());
						return false;
					}
					switch (*fmt) {
						case '3': dec.rd = ri->second; break;
						case '4': dec.rs1 = ri->second; break;
						case '5': dec.rs2 = ri->second; break;
						case '6': dec.rs2 = ri->second; break;
					}
					remove_operand(op_data, rv_type_freg);
					argv.pop_front();
					break;
				}
				case '7':
				{
					if (argv.size() == 0) {
						printf("%s missing immediate parameter\n", line->ref().c_str());
						return false;
					}
					auto arg = argv.front();
					auto result = eval(line, arg);
					dec.rs1 = result.asInt();
					remove_operand(op_data, rv_type_simm);
					remove_operand(op_data, rv_type_uimm);
					argv.pop_front();
					break;
				}
				case 'i':
				{
					if (argv.size() == 0) {
						printf("%s missing immediate parameter\n", line->ref().c_str());
						return false;
					}
					auto arg = argv.front();
					auto result = eval(line, arg);
					dec.imm = result.asInt();
					remove_operand(op_data, rv_type_simm);
					remove_operand(op_data, rv_type_uimm);
					argv.pop_front();
					break;
				}
				case 'o':
				{
					if (argv.size() == 0) {
						printf("%s missing immediate parameter\n", line->ref().c_str());
						return false;
					}
					auto arg = argv.front();
					auto result = eval(line, arg);
					dec.imm = result.asInt() - as.current_offset();
					remove_operand(op_data, rv_type_simm);
					argv.pop_front();
					break;
				}
				case 'c':
				{
					if (argv.size() == 0) {
						printf("%s missing csr parameter\n", line->ref().c_str());
						return false;
					}
					auto arg = argv.front();
					if (arg.size() != 1) {
						printf("%s invalid csr parameter\n", line->ref().c_str());
						return false;
					}
					s64 val;
					if (parse_integral(arg[0], val)) {
						dec.imm = val;
					} else {
						auto ci = csr_map.find(arg[0]);
						if (ci == csr_map.end()) {
							printf("%s unknown csr parameter\n", line->ref().c_str());
							return false;
						}
						dec.imm = ci->second;
					}
					remove_operand(op_data, rv_type_uimm);
					argv.pop_front();
					break;
				}
				case 'r':
					switch(dec.rm) {
						case rv_rm_rne: /* */; break;
						case rv_rm_rtz: /* */; break;
						case rv_rm_rdn: /* */; break;
						case rv_rm_rup: /* */; break;
						case rv_rm_rmm: /* */; break;
						case rv_rm_dyn: /* */; break;
					}
					break;
				case 'p':
					/* (dec.pred & rv_fence_i); */
					/* (dec.pred & rv_fence_o); */
					/* (dec.pred & rv_fence_r); */
					/* (dec.pred & rv_fence_w); */
					break;
				case 's':
					/* (dec.succ & rv_fence_i); */
					/* (dec.succ & rv_fence_o); */
					/* (dec.succ & rv_fence_r); */
					/* (dec.succ & rv_fence_w); */
					break;
				case 'O': break;
				case '\t': break;
				case 'A': /* if (dec.aq) */ break;
				case 'R': /* if (dec.rl) */ break;
				default:
					break;
			}
			fmt++;
		}
		/* translate pseudo instruction to regular instruction */
		encode_pseudo(dec);
		as.append(u32(encode_inst(dec)));
		return true;
	}

	bool handle_label(std::string label)
	{
		as.add_label(label);
		vars[label] = packToken(size_t(as.current_offset()));
		return true;
	}

	void write_elf(std::string filename)
	{
		/* TODO */
	}

	bool process_line(asm_line_ptr line)
	{
		if (debug) {
			printf("%-30s %s\n",
				format_string("%s:%05d", line->file->filename.c_str(), line->line_num).c_str(),
				join(line->args, " ").c_str());
		}

		/* check if we are defining a macro */
		if (defining_macro && !(line->args.size() > 0 && line->args[0] == ".endm")) {
			defining_macro->macro_lines.push_back(line);
			return true;
		}

		/* check for label */
		if (line->args.size() >= 2 && line->args[1] == ":") {
			handle_label(line->args[0]);
			line->args.erase(line->args.begin(), line->args.begin() + 2);
		}
		if (line->args.size() == 0) {
			return true;
		}

		/* check for internal directives */
		auto di = directive_map.find(line->args[0]);
		if (di != directive_map.end()) {
			if (!di->second(line)) {
				printf("%s invalid directive: %s\n",
					line->ref().c_str(), join(line->args, " ").c_str());
			}
			return true;
		}

		/* check for opcode */
		auto oi = opcode_map.find(line->args[0]);
		if (oi != opcode_map.end()) {
			if (!handle_opcode(oi->second, line)) {
				printf("%s invalid statement: %s\n",
					line->ref().c_str(), join(line->args, " ").c_str());
			}
			return true;
		}

		/* check for macro */
		auto mi = macro_map.find(line->args[0]);
		if (mi != macro_map.end()) {
			macro_stack.push_back(mi->second);
			for (auto macro_line : mi->second->macro_lines) {
				process_line(macro_substitute(macro_line));
			}
			macro_stack.pop_back();
			return true;
		}

		printf("%s unknown statement: %s\n",
			line->ref().c_str(), join(line->args, " ").c_str());

		return false;
	}

	void assemble()
	{
		/*
		 * TODO
		 *
		 * as allows mixed case
		 * as allows trailing semicolons
		 */

		std::vector<asm_line_ptr> data;
		read_source(data, input_filename);
		for (auto &line : data)
		{
			if (!process_line(line)) {
				if (bail_on_errors) {
					exit(9);
				}
			}
		}
		if (debug) {
			dump();
		}
		write_elf(output_filename);
	}

	void dump()
	{
		std::vector<u8> &buf = as.get_section(".text")->buf;
		addr_t pc = 0, end = buf.size();
		addr_t pc_offset;
		decode dec;
		while (pc < end) {
			inst_t inst = inst_fetch(addr_t(buf.data() + pc), pc_offset);
			decode_inst_rv64(dec, inst);
			decode_pseudo_inst(dec);
			std::string args = disasm_inst_simple(dec);
			printf("%16s\t%s\n", format_string("0x%llx", pc).c_str(), args.c_str());
			pc += pc_offset;
		}
	}
};

/* program main */

int main(int argc, const char* argv[])
{
	printf("\n");
	printf("rv-asm-0.0.0-prealpha-0\n");
	printf("\n");
	rv_assembler as;
	as.parse_commandline(argc, argv);
	as.assemble();
	return 0;
}