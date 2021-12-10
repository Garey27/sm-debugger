#include "debugger.h"
#include <assert.h>
#include <ctype.h>
#include <deque>
#include <setjmp.h>
#include <signal.h>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>
#include "utlbuffer.h"
#include <fstream>
#include <map>
#include <mutex>
#include <filesystem>

#include <brynet/net/EventLoop.hpp>
#include <brynet/net/ListenThread.hpp>
#include <brynet/net/PromiseReceive.hpp>
#include <brynet/net/SocketLibFunction.hpp>
#include <brynet/net/TcpService.hpp>
#include <brynet/net/http/HttpFormat.hpp>
#include <brynet/net/wrapper/ConnectionBuilder.hpp>
#include <brynet/net/wrapper/ServiceBuilder.hpp>

#include "sourcepawn/include/sp_vm_types.h"
#include <nlohmann/json.hpp>

using namespace sp;
using namespace brynet;
using namespace brynet::net;
using namespace brynet::net::http;

enum DebugState {
	DebugDead = -1,
	DebugRun = 0,
	DebugBreakpoint,
	DebugPause,
	DebugStepIn,
	DebugStepOver,
	DebugStepOut,
	DebugException
};
enum MessageType {
	Diagnostics = 0,
	RequestFile,
	File,

	StartDebugging,
	StopDebugging,
	Pause,
	Continue,

	RequestCallStack,
	CallStack,

	ClearBreakpoints,
	SetBreakpoint,

	HasStopped,
	HasContinued,

	StepOver,
	StepIn,
	StepOut,

	RequestSetVariable,
	SetVariable,
	RequestVariables,
	Variables,

	RequestEvaluate,
	Evaluate,

	Disconnect,
	TotalMessages
};

std::vector<std::string> split_string(const std::string& str,
	const std::string& delimiter) {
	std::vector<std::string> strings;

	std::string::size_type pos = 0;
	std::string::size_type prev = 0;
	while ((pos = str.find(delimiter, prev)) != std::string::npos) {
		strings.push_back(str.substr(prev, pos - prev));
		prev = pos + 1;
	}

	// To get the last substring (or only, if delimiter is not found)
	strings.push_back(str.substr(prev));

	return strings;
}
DebugReport DebugListener;
void removeClientID(const TcpConnection::Ptr& session);
class DebuggerClient {
public:
	TcpConnection::Ptr socket;
	std::set <std::string> files;
	int DebugState = 0;

	struct variable_s {
		std::string name;
		std::string value;
		std::string type;
	};

	struct call_stack_s {
		uint32_t line;
		std::string name;
		std::string filename;
	};

	struct breakpoint_s {
		long line;
		std::string filename;
	};

public:
	bool receive_walk_cmd = false;
	std::mutex mtx;
	std::condition_variable cv;
	SourcePawn::IPluginContext* context_;
	uint32_t current_line;
	std::map<std::string, std::set<long>> break_list;
	int current_state = 0;
	cell_t lastfrm_ = 0;
	cell_t cip_;
	cell_t frm_;
	std::map<std::string, SmxV1Image*> images;
	SmxV1Image* current_image = nullptr;
	SourcePawn::IFrameIterator* debug_iter;
	DebuggerClient(const TcpConnection::Ptr& tcp_connection)
		: socket(tcp_connection) {
	}

	~DebuggerClient() {
		stopDebugging();
		printf("Im dying!\n");
	}

	void setBreakpoint(std::string path, int line, int id) {
		break_list[path].insert(line);
	}

	void clearBreakpoints(std::string fileName) {
		auto found = break_list.find(fileName);
		if (found != break_list.end()) {
			found->second.clear();
		}
	}

	enum {
		DISP_DEFAULT = 0x10,
		DISP_STRING = 0x20,
		DISP_BIN = 0x30, /* ??? not implemented */
		DISP_HEX = 0x40,
		DISP_BOOL = 0x50,
		DISP_FIXED = 0x60,
		DISP_FLOAT = 0x70
	};
#define MAX_DIMS 3
#define DISP_MASK 0x0f

	char* get_string(SmxV1Image::Symbol* sym) {
		assert(sym->ident() == sp::IDENT_ARRAY ||
			sym->ident() == sp::IDENT_REFARRAY);
		assert(sym->dimcount() == 1);

		// get the starting address and the length of the string
		cell_t* addr;
		cell_t base = sym->addr();
		if (sym->vclass())
			base += frm_; // addresses of local vars are relative to the frame
		if (sym->ident() == sp::IDENT_REFARRAY) {
			context_->LocalToPhysAddr(base, &addr);
			assert(addr != nullptr);
			base = *addr;
		}

		char* str;
		if (context_->LocalToStringNULL(base, &str) != SP_ERROR_NONE)
			return nullptr;
		return str;
	}

	int get_symbolvalue(const SmxV1Image::Symbol* sym, int index,
		cell_t* value) {

		cell_t* vptr;
		cell_t base = sym->addr();
		if (sym->vclass() & DISP_MASK)
			base += frm_; // addresses of local vars are relative to the frame

		// a reference
		if (sym->ident() == sp::IDENT_REFERENCE ||
			sym->ident() == sp::IDENT_REFARRAY) {
			if (context_->LocalToPhysAddr(base, &vptr) != SP_ERROR_NONE)
				return false;

			assert(vptr != nullptr);
			base = *vptr;
		}

		if (context_->LocalToPhysAddr(base + index * sizeof(cell_t), &vptr) !=
			SP_ERROR_NONE)
			return false;

		if (vptr != nullptr)
			*value = *vptr;
		return vptr != nullptr;
	}

	void printvalue(long value, int disptype, std::string& out_value,
		std::string& out_type) {
		char out[64];
		if (disptype == DISP_FLOAT) {
			out_type = "float";
			sprintf(out, "%f", sp_ctof(value));
		}
		else if (disptype == DISP_FIXED) {
			out_type = "fixed";
#define MULTIPLIER 1000
			long ipart = value / MULTIPLIER;
			value -= MULTIPLIER * ipart;
			if (value < 0)
				value = -value;
			sprintf(out, "%ld.%03ld", ipart, value);
		}
		else if (disptype == DISP_HEX) {
			out_type = "hex";
			sprintf(out, "%lx", value);
		}
		else if (disptype == DISP_BOOL) {
			out_type = "bool";
			switch (value) {
			case 0:
				sprintf(out, "false");
				break;
			case 1:
				sprintf(out, "true");
				break;
			default:
				sprintf(out, "%ld (true)", value);
				break;
			} /* switch */
		}
		else {
			out_type = "cell";
			sprintf(out, "%ld", value);
		} /* if */
		out_value += out;
	}
	nlohmann::json read_variable(uint32_t addr, bool local, uint32_t type_id, debug::Rtti* rtti)
	{
		nlohmann::json json;
		if(!rtti)
		{
			rtti = const_cast<debug::Rtti*>(current_image->rtti_data()->typeFromTypeId(type_id));
		}
		cell_t* ptr;
		if (rtti->type() == cb::kEnumStruct)
		{
			auto fields = current_image->getEnumFields(rtti->index());

			size_t start{};

			start = addr;

			if (local)
				start += frm_;

			for (auto& field : fields)
			{
				auto name = current_image->GetDebugName(field->name);
				auto rtti_field = current_image->rtti_data()->typeFromTypeId(field->type_id);
				if (!rtti_field)
				{
					break;
				}
				switch (rtti_field->type())
				{
				case cb::kAny:
				{
					context_->LocalToPhysAddr(start, &ptr);
					json[name] = (int32_t)*ptr;
					start += sizeof(cell_t);
				}
				case cb::kBool:
				{
					context_->LocalToPhysAddr(start, &ptr);
					json[name] = (bool)*ptr;
					start += sizeof(cell_t);
					break;
				}
				case cb::kInt32:
				{
					context_->LocalToPhysAddr(start, &ptr);
					json[name] = (int32_t)*ptr;
					start += sizeof(cell_t);
					break;
				}
				case cb::kChar8:
				{
					context_->LocalToPhysAddr(start, &ptr);
					json[name] = *(uint8_t*)ptr;
					start += sizeof(cell_t);
					break;
				}
				case cb::kFloat32:
				{
					context_->LocalToPhysAddr(start, &ptr);
					json[name] = sp_ctof(*ptr);
					start += sizeof(cell_t);
					break;
				}
				case cb::kFixedArray:
				{
					if (rtti_field->inner())
					{
						switch (rtti_field->inner()->type())
						{
						case cb::kEnumStruct:
						{
							json[name] = read_variable(start, false, cb::kEnumStruct, const_cast<debug::Rtti*>(rtti_field->inner()));
							break;
						}
						}
					}
						break;
				}
				case cb::kArray:
				{
					if (rtti_field->inner())
					{
						switch (rtti_field->inner()->type())
						{
						case cb::kChar8:
						{
							char* str;
							if (context_->LocalToStringNULL(start, &str) != SP_ERROR_NONE)
							{
								break;
							}
							start += strlen(str);
							start += 1;
							if (start % sizeof(cell_t) != 0)
							{
								start += sizeof(cell_t) - (start % sizeof(cell_t));
							}
							json[name] = str;
							break;
						}
						case cb::kEnumStruct:
						{
							json[name] = read_variable(start, false, cb::kEnumStruct, const_cast<debug::Rtti*>(rtti_field->inner()));
							break;
						}
						}
					}
				}
				case cb::kEnum:
				case cb::kTypedef:
				case cb::kTypeset:
				case cb::kClassdef:
				{
					break;
				}
				case cb::kEnumStruct:
				{
					break;
				}
				}
			}
		}
		else if (rtti->type() == cb::kClassdef)
		{
			auto fields = current_image->getTypeFields(rtti->index());
			cell_t* ptr;
			size_t start{};
			if (context_->LocalToPhysAddr(addr, &ptr))
			{
				return json;
			}

			start = *ptr;
			
			for (auto& field : fields)
			{
				auto name = current_image->GetDebugName(field->name);
				auto rtti_field = current_image->rtti_data()->typeFromTypeId(field->type_id);
				switch (rtti_field->type())
				{
				case cb::kAny:
				{
					context_->LocalToPhysAddr(start, &ptr);
					start += sizeof(cell_t);
					json[name] = (int32_t)*ptr;
				}
				case cb::kBool:
				{
					context_->LocalToPhysAddr(start, &ptr);
					start += sizeof(cell_t);
					json[name] = (bool)*ptr;
					break;
				}
				case cb::kInt32:
				{
					context_->LocalToPhysAddr(start, &ptr);
					start += sizeof(cell_t);
					json[name] = (int32_t)*ptr;
					break;
				}
				case cb::kChar8:
				{
					context_->LocalToPhysAddr(start, &ptr);
					start += sizeof(cell_t);
					json[name] = *(uint8_t*)ptr;
					break;
				}
				case cb::kFloat32:
				{
					context_->LocalToPhysAddr(start, &ptr);
					start += sizeof(cell_t);
					json[name] = sp_ctof(*ptr);
					break;
				}
				case cb::kFixedArray:
				{
					if (rtti_field->inner())
					{
						switch (rtti_field->inner()->type())
						{
						case cb::kEnumStruct:
						{
							json[name] = read_variable(start, false, cb::kEnumStruct, const_cast<debug::Rtti*>(rtti_field->inner()));
							break;
						}
						}
					}
					break;
				}
				case cb::kArray:
				{
					if (rtti_field->inner())
					{
						switch (rtti_field->inner()->type())
						{
						case cb::kChar8:
						{
							char* str;
							if (context_->LocalToStringNULL(start, &str) != SP_ERROR_NONE)
							{
								break;
							}
							start += strlen(str);
							start += 1;
							if (start % sizeof(cell_t) != 0)
							{
								start += sizeof(cell_t) - (start % sizeof(cell_t));
							}
							json[name] = str;
							break;
						}
						case cb::kEnumStruct:
						{
							json[name] = read_variable(start, false, cb::kEnumStruct, const_cast<debug::Rtti*>(rtti_field->inner()));
							break;
						}
						}
					}
				}
				case cb::kEnum:
				case cb::kTypedef:
				case cb::kTypeset:
				case cb::kClassdef:
				{
					break;
				}
				case cb::kEnumStruct:
				{
					break;
				}
				}
			}
		}
		return json;
	}
	variable_s display_variable(SmxV1Image::Symbol* sym, uint32_t index[],
		int idxlevel, bool noarray = false) {
		nlohmann::json json;
		variable_s var;
		var.name = "N/A";
		if (current_image->GetDebugName(sym->name()) != nullptr) {
			var.name = current_image->GetDebugName(sym->name());
		};
		var.type = "N/A";
		var.value = "";
		cell_t value;
		std::unique_ptr<std::vector<SmxV1Image::ArrayDim*>> symdims;
		if (var.name == "myinfo")
		{
			printf("");
		}
		assert(index != NULL);
		auto rtti = sym->rtti();
		if (rtti && rtti->type_id)
		{
			auto json = read_variable(rtti->address, sym->vclass() & DISP_MASK, rtti->type_id, nullptr);
			if (!json.empty())
			{
				var.value = json.dump();
				return var;
			}
		}
		// first check whether the variable is visible at all
		if ((uint32_t)cip_ < sym->codestart() ||
			(uint32_t)cip_ > sym->codeend()) {
			var.value = "Not in scope.";
			return var;
		}

		// set default display type for the symbol (if none was set)
		if ((sym->vclass() & ~DISP_MASK) == 0) {
			const char* tagname = current_image->GetTagName(sym->tagid());
			if (tagname != nullptr) {
				if (!stricmp(tagname, "bool")) {
					sym->setVClass(sym->vclass() | DISP_BOOL);
				}
				else if (!stricmp(tagname, "float")) {
					sym->setVClass(sym->vclass() | DISP_FLOAT);
				}
			}
			if ((sym->vclass() & ~DISP_MASK) == 0 &&
				(sym->ident() == sp::IDENT_ARRAY ||
					sym->ident() == sp::IDENT_REFARRAY) &&
				sym->dimcount() == 1) {
				/* untagged array with a single dimension, walk through all
				 * elements and check whether this could be a string
				 */
				char* ptr = get_string(sym);
				if (ptr != nullptr) {
					uint32_t i;
					for (i = 0; ptr[i] != '\0'; i++) {
						if ((ptr[i] < ' ' && ptr[i] != '\n' && ptr[i] != '\r' &&
							ptr[i] != '\t'))
							break; // non-ASCII character
						if (i == 0 && !isalpha(ptr[i]))
							break; // want a letter at the start
					}
					if (i > 0 && ptr[i] == '\0')
						sym->setVClass(sym->vclass() | DISP_STRING);
				}
			}
		}

		if (sym->ident() == sp::IDENT_ARRAY ||
			sym->ident() == sp::IDENT_REFARRAY) {
			int dim;
			symdims = std::make_unique< std::vector<SmxV1Image::ArrayDim*>>(*current_image->GetArrayDimensions(sym));
			// check whether any of the indices are out of range
			assert(symdims != nullptr);
			for (dim = 0; dim < idxlevel; dim++) {
				if (symdims->at(dim)->size() > 0 &&
					index[dim] >= symdims->at(dim)->size())
					break;
			}
			if (dim < idxlevel) {

				var.value = "(index out of range)";
				return var;
			}
		}

		// Print first dimension of array
		if ((sym->ident() == sp::IDENT_ARRAY ||
			sym->ident() == sp::IDENT_REFARRAY) &&
			idxlevel == 0) {
			// Print string
			if ((sym->vclass() & ~DISP_MASK) == DISP_STRING) {
				var.type = "String";
				char* str = get_string(sym);
				if (str != nullptr)
				{
					var.value = str;
				}
				else
					var.value = "NULL_STRING";
			}
			// Print one-dimensional array
			else if (sym->dimcount() == 1) {

				if (!noarray)
					var.type = "Array";
				assert(symdims != nullptr); // set in the previous block
				uint32_t len = symdims->at(0)->size();
				uint32_t i;
				auto type = (sym->vclass() & ~DISP_MASK);
				if (type == DISP_FLOAT)
				{
					json = std::vector<float>();
				}
				else if (type == DISP_BOOL)
				{
					json = std::vector<bool>();
				}
				else
				{
					json = std::vector<cell_t>();
				}
				for (i = 0; i < len; i++) {
					if (get_symbolvalue(sym, i, &value))
					{
						if (type == DISP_FLOAT) {
							json.push_back(sp_ctof(value));
						}
						else if (type == DISP_BOOL)
						{
							json.push_back(value);
						}
						else
						{
							json.push_back(value);
						}
					}
				}
				var.value = json.dump(4).c_str();
			}
			// Not supported..
			else {
				var.value = "(multi-dimensional array)";
			}
		}
		else if (sym->ident() != sp::IDENT_ARRAY &&
			sym->ident() != sp::IDENT_REFARRAY && idxlevel > 0) {
			// index used on a non-array
			var.value = "(invalid index, not an array)";
		}
		else {
			// simple variable, or indexed array element
			assert(idxlevel > 0 ||
				index[0] == 0); // index should be zero if non-array
			int dim;
			int base = 0;
			for (dim = 0; dim < idxlevel - 1; dim++) {
				if (!noarray)
					var.type = "Array";
				base += index[dim];
				if (!get_symbolvalue(sym, base, &value))
					break;
				base += value / sizeof(cell_t);
			}

			if (get_symbolvalue(sym, base + index[dim], &value) &&
				sym->dimcount() == idxlevel)
				printvalue(value, (sym->vclass() & ~DISP_MASK), var.value,
					var.type);
			else if (sym->dimcount() != idxlevel)
				var.value = "(invalid number of dimensions)";
			else
				var.value = "(?)";
		}
		return var;
	}

	void evaluateVar(int frame_id, char* variable) {
		if (current_state != DebugRun) {
			SmxV1Image* imagev1 = (SmxV1Image*)current_image;

			std::unique_ptr<SmxV1Image::Symbol> sym;
			if (imagev1->GetVariable(variable, cip_, sym)) {
				uint32_t idx[MAX_DIMS], dim;
				dim = 0;
				memset(idx, 0, sizeof idx);
				auto var = display_variable(sym.get(), idx, dim);
				CUtlBuffer buffer;
				buffer.PutUnsignedInt(0);
				{
					buffer.PutChar(MessageType::Evaluate);
					buffer.PutInt(var.name.size() + 1);
					buffer.PutString(var.name.c_str());
					buffer.PutInt(var.value.size() + 1);
					buffer.PutString(var.value.c_str());
					;
					buffer.PutInt(var.type.size() + 1);
					buffer.PutString(var.type.c_str());
					buffer.PutInt(0);
				}
				*(uint32_t*)buffer.Base() = buffer.TellPut() - 5;
				socket->send(static_cast<const char*>(buffer.Base()),
					static_cast<size_t>(buffer.TellPut()));
			}
		}
	}

	int set_symbolvalue(const SmxV1Image::Symbol* sym, int index,
		cell_t value) {
		cell_t* vptr;
		cell_t base = sym->addr();
		if (sym->vclass() & DISP_MASK)
			base += frm_; // addresses of local vars are relative to the frame

		// a reference
		if (sym->ident() == sp::IDENT_REFERENCE ||
			sym->ident() == sp::IDENT_REFARRAY) {
			context_->LocalToPhysAddr(base, &vptr);
			assert(vptr != nullptr);
			base = *vptr;
		}

		context_->LocalToPhysAddr(base + index * sizeof(cell_t), &vptr);
		assert(vptr != nullptr);
		*vptr = value;
		return true;
	}

	bool SetSymbolString(const SmxV1Image::Symbol* sym, char* str) {
		assert(sym->ident() == sp::IDENT_ARRAY ||
			sym->ident() == sp::IDENT_REFARRAY);
		assert(sym->dimcount() == 1);

		cell_t* vptr;
		cell_t base = sym->addr();
		if (sym->vclass() & DISP_MASK)
			base += frm_; // addresses of local vars are relative to the frame

		// a reference
		if (sym->ident() == sp::IDENT_REFERENCE ||
			sym->ident() == sp::IDENT_REFARRAY) {
			context_->LocalToPhysAddr(base, &vptr);
			assert(vptr != nullptr);
			base = *vptr;
		}

		std::unique_ptr<std::vector<SmxV1Image::ArrayDim*>> dims;
		dims = std::make_unique<std::vector<SmxV1Image::ArrayDim*>>(*current_image->GetArrayDimensions(sym));
		return context_->StringToLocalUTF8(base, dims->at(0)->size(), str,
			NULL) == SP_ERROR_NONE;
	}

	void setVariable(std::string var, std::string value, int index) {
		bool success = false;
		bool valid_value = true;
		if (current_state != DebugRun) {
			SmxV1Image* imagev1 = (SmxV1Image*)current_image;
			std::unique_ptr<SmxV1Image::Symbol> sym;
			cell_t result = 0;
			value.erase(remove(value.begin(), value.end(), '\"'), value.end());
			if (imagev1->GetVariable(var.c_str(), cip_, sym)) {

				if ((sym->ident() == IDENT_ARRAY ||
					sym->ident() == IDENT_REFARRAY)) {
					if ((sym->vclass() & ~DISP_MASK) == DISP_STRING) {

						SetSymbolString(sym.get(), const_cast<char*>(value.c_str()));
					}
					valid_value = false;
				}
				else {
					size_t lastChar;
					try {
						int intvalue = std::stoi(value, &lastChar);
						if (lastChar == value.size()) {
							result = intvalue;
						}
						else {
							auto val = std::stof(value, &lastChar);
							result = sp_ftoc(val);
						}
					}
					catch (...) {
						// ??? some text or bool
						if (value == "true") {
							result = 1;
						}
						else if (value == "false") {
							result = 0;
						}
						else {
							valid_value = false;
						}
					}
				}

				if (valid_value &&
					(imagev1->GetVariable(var.c_str(), cip_, sym))) {
					success = set_symbolvalue(sym.get(), index, (cell_t)result);
				}
			}
		}
		CUtlBuffer buffer;
		buffer.PutUnsignedInt(0);
		{
			buffer.PutChar(MessageType::SetVariable);
			buffer.PutInt(success);
		}
		*(uint32_t*)buffer.Base() = buffer.TellPut() - 5;
		socket->send(static_cast<const char*>(buffer.Base()),
			static_cast<size_t>(buffer.TellPut()));
	}

	void sendVariables(char* scope) {
		bool local_scope = strstr(scope, ":%local%");
		bool global_scope = strstr(scope, ":%global%");
		if (current_state != DebugRun) {
			SmxV1Image* imagev1 = (SmxV1Image*)current_image;

			std::unique_ptr<SmxV1Image::Symbol> sym;
			if (current_image && imagev1) {
#define sDIMEN_MAX 4
				uint32_t idx[sDIMEN_MAX], dim;
				dim = 0;
				memset(idx, 0, sizeof idx);
				std::vector<variable_s> vars;
				if (local_scope || global_scope) {
					SmxV1Image::SymbolIterator iter = imagev1->symboliterator(global_scope);
					while (!iter.Done()) {
						const auto sym = iter.Next();

						// Only variables in scope.
						if (sym->ident() != sp::IDENT_FUNCTION &&
							(sym->codestart() <= (uint32_t)cip_ &&
								sym->codeend() >= (uint32_t)cip_) || global_scope) {
							auto var = display_variable(sym, idx, dim);
							if (local_scope) {
								if ((sym->vclass() & DISP_MASK) > 0) {
									vars.push_back(var);
								}
							}
							else {
								if (!((sym->vclass() & DISP_MASK) > 0)) {
									vars.push_back(var);
								}
							}
						}
					}
				}
				else {
					if (imagev1->GetVariable(scope, cip_, sym)) {
						auto var = display_variable(sym.get(), idx, dim, true);
						std::string var_name = scope;
						auto values = split_string(var.value, ",");
						int i = 0;
						for (auto val : values) {
							vars.push_back({ std::to_string(i), val, var.type });
							i++;
						}
					}
				}
				CUtlBuffer buffer;
				buffer.PutUnsignedInt(0);
				buffer.PutChar(Variables);
				buffer.PutInt(strlen(scope) + 1);
				buffer.PutString(scope);
				buffer.PutInt(vars.size());
				for (auto var : vars) {
					buffer.PutInt(var.name.size() + 1);
					buffer.PutString(var.name.c_str());
					buffer.PutInt(var.value.size() + 1);
					buffer.PutString(var.value.c_str());
					;
					buffer.PutInt(var.type.size() + 1);
					buffer.PutString(var.type.c_str());
					buffer.PutInt(0);
				}
				*(uint32_t*)buffer.Base() = buffer.TellPut() - 5;
				socket->send(static_cast<const char*>(buffer.Base()),
					static_cast<size_t>(buffer.TellPut()));
			}
		}
	}

	void CallStack() {
		std::vector<call_stack_s> callStack;
		if (current_state == DebugException) {
			if (debug_iter) {
				uint32_t index = 0;
				for (; !debug_iter->Done(); debug_iter->Next(), index++) {

					if (debug_iter->IsNativeFrame()) {
						continue;
					}

					if (debug_iter->IsScriptedFrame()) {
						auto current_file = std::filesystem::path(debug_iter->FilePath()).filename().string();
						std::ranges::transform(current_file, current_file.begin(),
							[](unsigned char c) { return std::tolower(c); });
						callStack.push_back({ debug_iter->LineNumber() - 1,
											 debug_iter->FunctionName(),
											 current_file });
					}
				}
			}
			current_state = DebugBreakpoint;
		}
		else if (current_state != DebugRun) {

			IFrameIterator* iter = context_->CreateFrameIterator();

			uint32_t index = 0;
			for (; !iter->Done(); iter->Next(), index++) {

				if (iter->IsNativeFrame()) {
					continue;
				}

				if (iter->IsScriptedFrame()) {
					std::string current_file = iter->FilePath();
					for (auto file : files) {
						if (file.find(current_file) != std::string::npos) {
							current_file = file;
							break;
						}
					}
					callStack.push_back({ iter->LineNumber() - 1,
										 iter->FunctionName(), current_file });
				}
			}
			context_->DestroyFrameIterator(iter);
			/*if (!callStack.empty()) {
				callStack[0].line = current_line - 1;
				callStack[0].filename = current_file;
			} else {
				callStack.push_back({current_line - 1,
									 std::to_string(current_line - 1),
									 current_file});
			}*/
		}

		CUtlBuffer buffer;
		buffer.PutUnsignedInt(0);
		{
			buffer.PutChar(MessageType::CallStack);
			buffer.PutInt(callStack.size());
			for (auto stack : callStack) {
				buffer.PutInt(stack.name.size() + 1);
				buffer.PutString(stack.name.c_str());
				buffer.PutInt(stack.filename.size() + 1);
				buffer.PutString(stack.filename.c_str());
				buffer.PutInt(stack.line + 1);
			}
		}
		*(uint32_t*)buffer.Base() = buffer.TellPut() - 5;
		socket->send(static_cast<const char*>(buffer.Base()),
			static_cast<size_t>(buffer.TellPut()));
	}

	void WaitWalkCmd(std::string reason = "Breakpoint",
		std::string text = "N/A") {
		if (!receive_walk_cmd) {
			CUtlBuffer buffer;
			{
				buffer.PutUnsignedInt(0);
				{
					buffer.PutChar(MessageType::HasStopped);
					buffer.PutInt(reason.size() + 1);
					buffer.PutString(reason.c_str());
					buffer.PutInt(reason.size() + 1);
					buffer.PutString(reason.c_str());
					buffer.PutInt(text.size() + 1);
					buffer.PutString(text.c_str());
				}
				*(uint32_t*)buffer.Base() = buffer.TellPut() - 5;
			}
			socket->send(static_cast<const char*>(buffer.Base()),
				static_cast<size_t>(buffer.TellPut()));
			std::unique_lock<std::mutex> lck(mtx);
			cv.wait(lck, [this] { return receive_walk_cmd; });
		}
	}

	void ReportError(const IErrorReport& report, IFrameIterator& iter) {
		receive_walk_cmd = false;
		current_state = DebugException;
		context_ = iter.Context();
		debug_iter = &iter;
		WaitWalkCmd("exception", report.Message());
	}
	int(DebugHook)(SourcePawn::IPluginContext* ctx,
		sp_debug_break_info_t& BreakInfo) {
		std::string filename = ctx->GetRuntime()->GetFilename();
		auto image = images.find(filename);
		if (image == images.end()) {
			FILE* fp = fopen(filename.c_str(), "rb");
			current_image = new SmxV1Image(fp);
			current_image->validate();
			images.insert({ filename, current_image });
			fclose(fp);
		}
		else {
			current_image = image->second;
		}
		context_ = ctx;
		if (current_state == DebugDead)
			return current_state;

		context_ = ctx;
		cip_ = BreakInfo.cip;
		// Reset the state.
		frm_ = BreakInfo.frm;
		receive_walk_cmd = false;

		IFrameIterator* iter = context_->CreateFrameIterator();
		std::string current_file = "N/A";
		uint32_t index = 0;
		for (; !iter->Done(); iter->Next(), index++) {

			if (iter->IsNativeFrame()) {
				continue;
			}

			if (iter->IsScriptedFrame()) {
				current_file = std::filesystem::path(iter->FilePath()).filename().string();
				std::ranges::transform(current_file, current_file.begin(),
					[](unsigned char c) { return std::tolower(c); });

				for (auto file : files) {
					if (file.find(current_file) != std::string::npos) {
						current_file = file;
						break;
					}
				}
				break;
			}
		}
		context_->DestroyFrameIterator(iter);

		static long lastline = 0;
		current_image->LookupLine(cip_, &current_line);
		// Reset the frame iterator, so stack traces start at the beginning
		// again.

		/* dont break twice */
		if (current_line == lastline)
			return current_state;

		lastline = current_line;
		if (current_state == DebugStepOut && frm_ > lastfrm_)
			current_state = DebugStepIn;

		if (current_state == DebugPause || current_state == DebugStepIn) {
			WaitWalkCmd();
		}
		else {

			auto found = break_list.find(current_file);
			if (found != break_list.end()) {
				for (auto br : found->second) {
					if (current_line == br) {
						current_line = br;
						current_state = DebugBreakpoint;
						WaitWalkCmd();
						break;
					}
				}
			}
		}

		/* check whether we are stepping through a sub-function */
		if (current_state == DebugStepOver) {
			if (frm_ < lastfrm_)
				return current_state;
			else
				WaitWalkCmd();
			if (current_state == DebugDead)
				return DebugDead;
		}

		lastfrm_ = frm_;

		return current_state;
	}

	void SwitchState(unsigned char state) {
		current_state = state;
		receive_walk_cmd = true;
		cv.notify_one();
	}

	void AskFile() {
	}

	void RecvDebugFile(CUtlBuffer* buf) {
		char file[260];
		int strlen = buf->GetInt();
		buf->GetString(file, strlen);
		auto filename = std::filesystem::path(file).filename().string();
		std::ranges::transform(filename, filename.begin(),
			[](unsigned char c) { return std::tolower(c); });
		files.insert(filename);
	}

	void RecvStateSwitch(CUtlBuffer* buf) {
		auto CurrentState = buf->GetUnsignedChar();
		SwitchState(CurrentState);
	}

	void RecvCallStack(CUtlBuffer* buf) {
		CallStack();
	}

	void recvRequestVariables(CUtlBuffer* buf) {
		char scope[256];
		int strlen = buf->GetInt();
		buf->GetString(scope, strlen);
		sendVariables(scope);
	}

	void recvRequestEvaluate(CUtlBuffer* buf) {
		int frameId;
		char variable[256];
		int strlen = buf->GetInt();
		buf->GetString(variable, strlen);
		frameId = buf->GetInt();
		evaluateVar(frameId, variable);
	}

	void recvDisconnect(CUtlBuffer* buf) {
	}

	void recvBreakpoint(CUtlBuffer* buf) {
		char path[256];
		int strlen = buf->GetInt();
		buf->GetString(path, strlen);
		std::string filename(std::filesystem::path(path).filename().string());
		std::ranges::transform(filename, filename.begin(),
			[](unsigned char c) { return std::tolower(c); });
		files.insert(filename);
		int line = buf->GetInt();
		int id = buf->GetInt();
		setBreakpoint(filename, line, id);
	}

	void recvClearBreakpoints(CUtlBuffer* buf) {
		char path[256];
		int strlen = buf->GetInt();
		buf->GetString(path, strlen);

		std::string filename(std::filesystem::path(path).filename().string());
		std::ranges::transform(filename, filename.begin(),
			[](unsigned char c) { return std::tolower(c); });
		clearBreakpoints(filename);
	}

	void stopDebugging() {
		if (!receive_walk_cmd) {
			current_state = DebugDead;
			receive_walk_cmd = true;
			cv.notify_one();
		}
	}

	void recvStopDebugging(CUtlBuffer* buf) {
		stopDebugging();
		removeClientID(socket);
	}

	void recvRequestSetVariable(CUtlBuffer* buf) {
		char var[256];
		int strlen = buf->GetInt();
		buf->GetString(var, strlen);
		char value[256];
		strlen = buf->GetInt();
		buf->GetString(value, strlen);
		auto index = buf->GetInt();
		setVariable(var, value, index);
	}

	void RecvCmd(const char* buffer, size_t len) {
		CUtlBuffer buf((void*)buffer, len);
		while (buf.TellGet() < len) {
			int msg_len = buf.GetUnsignedInt();
			int type = buf.GetUnsignedChar();
			switch (type) {
			case RequestFile: {
				RecvDebugFile(&buf);
				break;
			}
			case Pause: {
				RecvStateSwitch(&buf);
				break;
			}
			case Continue: {
				RecvStateSwitch(&buf);
				break;
			}
			case StepIn: {
				RecvStateSwitch(&buf);
				break;
			}
			case StepOver: {
				RecvStateSwitch(&buf);
				break;
			}
			case StepOut: {
				RecvStateSwitch(&buf);
				break;
			}
			case RequestCallStack: {
				RecvCallStack(&buf);
				break;
			}
			case RequestVariables: {
				recvRequestVariables(&buf);
				break;
			}
			case RequestEvaluate: {
				recvRequestEvaluate(&buf);
				break;
			}
			case Disconnect: {
				recvDisconnect(&buf);
				break;
			}
			case ClearBreakpoints: {
				recvClearBreakpoints(&buf);
				break;
			}
			case SetBreakpoint: {
				recvBreakpoint(&buf);
				break;
			}
			case StopDebugging: {
				recvStopDebugging(&buf);
				break;
			}
			case RequestSetVariable: {
				recvRequestSetVariable(&buf);
				break;
			}
			}
		}
	}
};

std::vector<std::unique_ptr<DebuggerClient>> clients;

void addClientID(const TcpConnection::Ptr& session) {
	clients.push_back(std::make_unique<DebuggerClient>(session));
	clients.back()->AskFile();
}

void removeClientID(const TcpConnection::Ptr& session) {
	for (auto it = clients.begin(); it != clients.end(); ++it) {
		if ((*it)->socket == session) {
			clients.erase(it);
			break;
		}
	}
}


void debugThread() {
	auto service = TcpService::Create();
	service->startWorkerThread(2);

	auto mainLoop = std::make_shared<EventLoop>();
	auto enterCallback = [=](const TcpConnection::Ptr& session) {
		addClientID(session);
		session->setDisConnectCallback([=](
			const TcpConnection::Ptr& session) {
				removeClientID(session);
			});
		auto contentLength = std::make_shared<size_t>();
		session->setDataCallback([=](brynet::base::BasePacketReader& reader) {
			for (auto& client : clients) {
				if (client->socket == session) {
					client->RecvCmd(reader.begin(), reader.size());
					break;
				}
			}
			reader.consumeAll();
			});
	};

	wrapper::ListenerBuilder listener;
	listener.WithService(service)
		.AddSocketProcess(
			{ [](TcpSocket& socket) { socket.setNodelay(); } })
		.WithMaxRecvBufferSize(1024 * 1024)
		.AddEnterCallback(enterCallback)
		.WithAddr(false, "0.0.0.0", SM_Debugger_port())
		.asyncRun();

	while (true) {
		mainLoop->loop(1000);
	}
}

/**
 * @brief Called on debug spew.
 *
 * @param msg    Message text.
 * @param fmt    Message formatting arguments (printf-style).
 */
void DebugReport::OnDebugSpew(const char* msg, ...) {
	va_list ap;
	char buffer[512];

	va_start(ap, msg);
	ke::SafeVsprintf(buffer, sizeof(buffer), msg, ap);
	va_end(ap);
	original->OnDebugSpew(buffer);
}

/**
 * @brief Called when an error is reported and no exception
 * handler was available.
 *
 * @param report  Error report object.
 * @param iter      Stack frame iterator.
 */
void DebugReport::ReportError(const IErrorReport& report,
	IFrameIterator& iter) {
	if (!clients.empty()) {
		auto plugin = report.Context();
		if (plugin) {

			auto found = false;
			/* first search already found attached hook */
			for (auto& client : clients) {
				if (client && client->context_ == iter.Context()) {
					found = true;
					client->ReportError(report, iter);
					break;
				}
			}

			/* if not found, search for new client who wants to attach to
			 * current file */
			if (!found) {
				for (auto& client : clients) {
					for (int i = 0; i < report.Context()
						->GetRuntime()
						->GetDebugInfo()
						->NumFiles();
						i++) {
						for (auto file : client->files) {
							auto filename = report.Context()
								->GetRuntime()
								->GetDebugInfo()
								->GetFileName(i);
							if (file.find(filename) != std::string::npos ||
								strcmpi(file.c_str(), filename) == 0) {
								client->ReportError(report, iter);
								break;
							}
						}
					}
				}
			}
		}
	}

	original->ReportError(report, iter);
}

void(DebugHandler)(SourcePawn::IPluginContext* IPlugin,
	sp_debug_break_info_t& BreakInfo,
	const SourcePawn::IErrorReport* IErrorReport) {
	if (!IPlugin->IsDebugging())
		return;

	if (!clients.empty()) {
		auto found = false;
		/* first search already found attached hook */
		for (auto& client : clients) {
			if (client && client->context_ == IPlugin) {
				found = true;
				client->DebugHook(IPlugin, BreakInfo);
				break;
			}
		}

		/* if not found, search for new client who wants to attach to current
		 * file */
		if (!found) {
			for (auto& client : clients) {
				for (int i = 0;
					i < IPlugin->GetRuntime()->GetDebugInfo()->NumFiles();
					i++) {
					auto filename =
						IPlugin->GetRuntime()->GetDebugInfo()->GetFileName(
							i);

					auto current_file = std::filesystem::path(filename).filename().string();
					std::ranges::transform(current_file, current_file.begin(),
						[](unsigned char c) { return std::tolower(c); });
					if (client->files.find(current_file) != client->files.end())
					{
						client->DebugHook(IPlugin, BreakInfo);
						break;
					}
				}
			}
		}
	}
}
