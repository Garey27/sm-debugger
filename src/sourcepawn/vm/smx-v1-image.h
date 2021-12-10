// vim: set sts=2 ts=8 sw=2 tw=99 et:
//
// Copyright (C) 2004-2015 AlliedModers LLC
//
// This file is part of SourcePawn. SourcePawn is licensed under the GNU
// General Public License, version 3.0 (GPL). If a copy of the GPL was not
// provided with this file, you can obtain it here:
//   http://www.gnu.org/licenses/gpl.html
//
#ifndef _include_sourcepawn_smx_parser_h_
#define _include_sourcepawn_smx_parser_h_
#include <am-string.h>
#include <am-vector.h>
#include <smx/smx-headers.h>
#include <smx/smx-v1.h>
#include <sp_vm_types.h>
#include <stdio.h>
#include "file-utils.h"
#include "legacy-image.h"
#include "smx/smx-legacy-debuginfo.h"
#include "smx/smx-typeinfo.h"
#include <functional>
#include "rtti.h"
namespace sp {

class SmxV1Image : public FileReader
{
    struct Section {
        const char* name;
        uint32_t dataoffs;
        uint32_t size;
    };

  public:
    SmxV1Image(FILE* fp);

    // This must be called to initialize the reader.
    bool validate();

    const sp_file_hdr_t* hdr() const {
        return hdr_;
    }

    const char* errorMessage() const {
        return error_.c_str();
    }

  public:
    LegacyImage::Code DescribeCode() const;
    LegacyImage::Data DescribeData() const;
    size_t NumNatives() const;
    const char* GetNative(size_t index) const;
    bool FindNative(const char* name, size_t* indexp) const;
    size_t NumPublics() const;
    void GetPublic(size_t index, uint32_t* offsetp, const char** namep) const;
    bool FindPublic(const char* name, size_t* indexp) const;
    size_t NumPubvars() const;
    void GetPubvar(size_t index, uint32_t* offsetp, const char** namep) const;
    bool FindPubvar(const char* name, size_t* indexp) const;
    size_t HeapSize() const;
    size_t ImageSize() const;
    const char* LookupFile(uint32_t code_offset);
    const char* LookupFunction(uint32_t code_offset);
    bool LookupLine(uint32_t code_offset, uint32_t* line);

    // Additional information for interactive debugging.
    class Symbol;
    bool GetFunctionAddress(const char* function, const char* file, uint32_t* addr);
    bool GetLineAddress(const uint32_t line, const char* file, uint32_t* addr);
    const char* FindFileByPartialName(const char* partialname);
    bool GetVariable(const char* symname, uint32_t scopeaddr, std::unique_ptr<Symbol>& sym);
    const char* GetDebugName(uint32_t nameoffs);
    const char* GetFileName(uint32_t index);
    uint32_t GetFileCount();

  public:
    const char* GetTagName(uint32_t tag);

  public:
    class Symbol
    {
      public:
        enum { VAR_PACKED, VAR_UNPACKED, VAR_RTTI };
        Symbol(sp_fdbg_symbol_t* sym, SmxV1Image* image)
         : addr_(sym->addr)
         , tagid_(sym->tagid)
         , codestart_(sym->codestart)
         , codeend_(sym->codeend)
         , ident_(sym->ident)
         , vclass_(sym->vclass)
         , dimcount_(sym->dimcount)
         , name_(sym->name)
         , sym_(sym)
         , type_(VAR_PACKED)
         , unpacked_sym_(nullptr) {
        }

        Symbol(sp_u_fdbg_symbol_t* sym, SmxV1Image* image)
         : addr_(sym->addr)
         , tagid_(sym->tagid)
         , codestart_(sym->codestart)
         , codeend_(sym->codeend)
         , ident_(sym->ident)
         , vclass_(sym->vclass)
         , dimcount_(sym->dimcount)
         , name_(sym->name)
         , type_(VAR_UNPACKED)
         , sym_(nullptr)
         , unpacked_sym_(sym) {
        }

        Symbol(smx_rtti_debug_var* sym, SmxV1Image* image)
         : addr_(sym->address)
         , codestart_(sym->code_start)
         , codeend_(sym->code_end)
         , name_(sym->name)
         , type_(VAR_RTTI)
         , sym_(nullptr)
         , unpacked_sym_(nullptr)
         , rtti_sym(sym) {
            dimcount_ = 0;
            enum {
                DISP_DEFAULT = 0x10,
                DISP_STRING = 0x20,
                DISP_BIN = 0x30, /* ??? not implemented */
                DISP_HEX = 0x40,
                DISP_BOOL = 0x50,
                DISP_FIXED = 0x60,
                DISP_FLOAT = 0x70
            };

            auto DecodeUint32 = [](unsigned char* bytes, int& offset) {
                uint32_t value = 0;
                int shift = 0;
                for (;;) {
                    unsigned char b = bytes[offset++];
                    value |= (uint32_t)(b & 0x7f) << shift;
                    if ((b & 0x80) == 0)
                        break;
                    shift += 7;
                }
                return (int)value;
            };
            std::function<void(unsigned char*, int&)> Decode;
            Decode = [this, DecodeUint32, &Decode](unsigned char* bytes, int& offset) {
                unsigned char b = bytes[offset++];
                switch (b) {
                    case cb::kFixedArray: {
                        ident_ = IDENT_ARRAY;
                        DecodeUint32(bytes, offset);
                        dimcount_++;
                        Decode(bytes, offset);
                        break;
                    }
                }
            };
            vclass_ = sym->vclass;
            int kind = (sym->type_id) & 0xf;
            int payload = ((sym->type_id) >> 4) & 0xfffffff;
            if (kind == kTypeId_Inline) {
                unsigned char temp[4];
                temp[0] = (payload & 0xff);
                temp[1] = ((payload >> 8) & 0xff);
                temp[2] = ((payload >> 16) & 0xff);
                temp[3] = ((payload >> 24) & 0xff);
                int offset = 0;
                Decode(temp, offset);
            }
        }
        Symbol(Symbol* sym)
         : addr_(sym->addr_)
         , tagid_(sym->tagid_)
         , codestart_(sym->codestart_)
         , codeend_(sym->codeend_)
         , ident_(sym->ident_)
         , vclass_(sym->vclass_)
         , dimcount_(sym->dimcount_)
         , name_(sym->name_)
         , sym_(sym->sym_)
         , type_(sym->type_)
         , rtti_sym(sym->rtti_sym)
         , unpacked_sym_(sym->unpacked_sym_) {
        }

        const int32_t addr() const {
            return addr_;
        }
        const int16_t tagid() const {
            return tagid_;
        }
        const uint32_t codestart() const {
            return codestart_;
        }
        const uint32_t codeend() const {
            return codeend_;
        }
        const uint8_t ident() const {
            return ident_;
        }
        const uint8_t vclass() const {
            return vclass_;
        }
        const uint16_t dimcount() const {
            return dimcount_;
        }
        const uint32_t name() const {
            return name_;
        }
        void setVClass(uint8_t vclass) {
            vclass_ = vclass;
            if (sym_)
                sym_->vclass = vclass;
            else if (unpacked_sym_)
                unpacked_sym_->vclass = vclass;
            else
                rtti_sym->vclass = vclass;
        }
        const bool packed() const {
            return sym_ != nullptr;
        }
        const uint8_t type() const {
            return type_;
        }
        const smx_rtti_debug_var* rtti() const {
            return rtti_sym;
        }
        const void* sym() const {
            if (sym_) {
                return sym_;
            }
            if (sym_) {
                return unpacked_sym_;
            }
            if (rtti_sym) {
                return rtti_sym;
            }
        }

      private:
        int32_t addr_;       /**< Address rel to DAT or stack frame */
        int16_t tagid_;      /**< Tag id */
        uint32_t codestart_; /**< Start scope validity in code */
        uint32_t codeend_;   /**< End scope validity in code */
        uint8_t ident_;      /**< Variable type */
        uint8_t vclass_;     /**< Scope class (local vs global) */
        uint16_t dimcount_;  /**< Dimension count (for arrays) */
        uint32_t name_;      /**< Offset into debug nametable */
        uint8_t type_;

        sp_fdbg_symbol_t* sym_;
        sp_u_fdbg_symbol_t* unpacked_sym_;
        smx_rtti_debug_var* rtti_sym;
    };

    class SymbolIterator
    {
      public:
        SymbolIterator(uint8_t* start, uint32_t debug_symbols_section_size, int type,
                       SmxV1Image* image)
         : cursor_(start)
         , type_(type)
         , image_(image) {
            index_ = 0;
            cursor_end_ = cursor_ + debug_symbols_section_size;
        }

        bool Done() {
            if (type_ == 1) {
                return cursor_ + sizeof(sp_fdbg_symbol_t) > cursor_end_;
            } else if (type_ == 0) {
                return cursor_ + sizeof(sp_u_fdbg_symbol_t) > cursor_end_;
            } else if (type_ == 2) {                    
                return index_ >= image_->locals_->row_count;
            } else 
            { return index_ >= image_->globals_->row_count; }
        }

        Symbol* Next() {
            if (type_ == 1) {
                sp_fdbg_symbol_t* sym = reinterpret_cast<sp_fdbg_symbol_t*>(cursor_);
                if (sym->dimcount > 0)
                    cursor_ += sizeof(sp_fdbg_arraydim_t) * sym->dimcount;
                cursor_ += sizeof(sp_fdbg_symbol_t);

                return new Symbol(sym, nullptr);
            } else if (type_ == 0) {
                sp_u_fdbg_symbol_t* sym = reinterpret_cast<sp_u_fdbg_symbol_t*>(cursor_);
                if (sym->dimcount > 0)
                    cursor_ += sizeof(sp_u_fdbg_arraydim_t) * sym->dimcount;
                cursor_ += sizeof(sp_u_fdbg_symbol_t);

                return new Symbol(sym, nullptr);
            } else {
                const smx_rtti_debug_var* sym = image_->getRttiRow<smx_rtti_debug_var>(
                    (type_ == 2) ? image_->locals_ : image_->globals_, index_);
                //smx_rtti_debug_var* sym = reinterpret_cast<smx_rtti_debug_var*>(cursor_);
                index_ += 1;
                return new Symbol((smx_rtti_debug_var*)sym, image_);
            }
        }

      private:
        uint8_t* cursor_;
        uint8_t* cursor_end_;
        uint32_t index_;
        int type_;
        SmxV1Image* image_;
    };

    SymbolIterator symboliterator(bool global = false);

    class ArrayDim
    {
      public:
        ArrayDim(sp_fdbg_arraydim_t* dim)
         : tagid_(dim->tagid)
         , size_(dim->size) {
        }

        ArrayDim(sp_u_fdbg_arraydim_t* dim)
         : tagid_(dim->tagid)
         , size_(dim->size) {
        }
        ArrayDim(uint32_t size)
         : size_(size) {
        }

        int16_t tagid() {
            return tagid_;
        }
        uint32_t size() {
            return size_;
        }

      private:
        int16_t tagid_; /**< Tag id */
        uint32_t size_; /**< Size of dimension */
    };

   std::vector<ArrayDim*>* GetArrayDimensions(const Symbol* sym);
    bool validateRttiField(uint32_t index);
    size_t getTypeFromTypeId(uint32_t typeId);
    std::vector<smx_rtti_es_field*> getEnumFields(uint32_t index);
    size_t getTypeSize(uint32_t typeId);
    std::vector<smx_rtti_field*> getTypeFields(uint32_t typeId);
    bool validateRttiClassdefs();
    bool validateRttiEnums();
    bool validateRttiEnumStructField(const smx_rtti_enumstruct* enumstruct, uint32_t index);
    bool validateRttiEnumStructs();

private:
    const Section* findSection(const char* name);

  public:
    template <typename T>
    class Blob
    {
      public:
        Blob()
         : header_(nullptr)
         , section_(nullptr)
         , blob_(nullptr)
         , length_(0)
         , features_(0) {
        }
        Blob(const Section* header, const T* section, const uint8_t* blob, size_t length,
             uint32_t features)
         : header_(header)
         , section_(section)
         , blob_(blob)
         , length_(length)
         , features_(features) {
        }

        size_t size() const {
            return section_->size;
        }
        const T* operator->() const {
            return section_;
        }
        const uint8_t* blob() const {
            return blob_;
        }
        size_t length() const {
            return length_;
        }
        bool exists() const {
            return !!header_;
        }
        uint32_t features() const {
            return features_;
        }
        const Section* header() const {
            return header_;
        }

      private:
        const Section* header_;
        const T* section_;
        const uint8_t* blob_;
        size_t length_;
        uint32_t features_;
    };

    template <typename T>
    class List
    {
      public:
        List()
         : section_(nullptr)
         , length_(0) {
        }
        List(const T* section, size_t length)
         : section_(section)
         , length_(length) {
        }

        size_t length() const {
            return length_;
        }
        const T& operator[](size_t index) const {
            assert(index < length());
            return section_[index];
        }
        bool exists() const {
            return !!section_;
        }

      private:
        const T* section_;
        size_t length_;
    };

  public:
    const Blob<sp_file_code_t>& code() const {
        return code_;
    }
    const Blob<sp_file_data_t>& data() const {
        return data_;
    }
    const List<sp_file_publics_t>& publics() const {
        return publics_;
    }
    const List<sp_file_natives_t>& natives() const {
        return natives_;
    }
    const List<sp_file_pubvars_t>& pubvars() const {
        return pubvars_;
    }
    const List<sp_file_tag_t>& tags() const {
        return tags_;
    }

    std::unique_ptr<const debug::RttiData>& rtti_data() {
        return rtti_data_;
    }
  protected:
    bool error(const char* msg) {
        error_ = msg;
        return false;
    }
    bool validateName(size_t offset);
    bool validateRtti();
    bool validateRttiMethods();
    bool validateRttiHeader(const Section* section);
    bool validateSection(const Section* section);
    bool validateCode();
    bool validateData();
    bool validatePublics();
    bool validatePubvars();
    bool validateNatives();
    bool validateDebugInfo();
    bool validateTags();

  private:
    template <typename SymbolType, typename DimType>
    const char* lookupFunction(const SymbolType* syms, uint32_t addr);
    template <typename SymbolType, typename DimType>
    bool getFunctionAddress(const SymbolType* syms, const char* name, uint32_t* addr,
                            uint32_t* index);

    const smx_rtti_table_header* findRttiSection(const char* name) {
        const Section* section = findSection(name);
        if (!section)
            return nullptr;
        return reinterpret_cast<const smx_rtti_table_header*>(buffer() + section->dataoffs);
    }

    template <typename T>
    const T* getRttiRow(const smx_rtti_table_header* header, size_t index) {
        assert(index < header->row_count);
        const uint8_t* base = reinterpret_cast<const uint8_t*>(header) + header->header_size;
        return reinterpret_cast<const T*>(base + header->row_size * index);
    }

  private:
    sp_file_hdr_t* hdr_;
    std::string error_;
    const char* header_strings_;
   std::vector<Section> sections_;

    const Section* names_section_;
    const char* names_;

    Blob<sp_file_code_t> code_;
    Blob<sp_file_data_t> data_;
    List<sp_file_publics_t> publics_;
    List<sp_file_natives_t> natives_;
    List<sp_file_pubvars_t> pubvars_;
    List<sp_file_tag_t> tags_;

    const Section* debug_names_section_;
    const char* debug_names_;
    const sp_fdbg_info_t* debug_info_;
    List<sp_fdbg_file_t> debug_files_;
    List<sp_fdbg_line_t> debug_lines_;
    const Section* debug_symbols_section_;
    const sp_fdbg_symbol_t* debug_syms_;
    const sp_u_fdbg_symbol_t* debug_syms_unpacked_;

    std::unique_ptr<const debug::RttiData> rtti_data_ = nullptr;
    const smx_rtti_table_header* rtti_fields_ = nullptr;
    const smx_rtti_table_header* rtti_methods_;
    const smx_rtti_table_header* rtti_classdefs_;
    const smx_rtti_table_header* globals_;
    const smx_rtti_table_header* locals_;
    const smx_rtti_table_header* methods_;
    const smx_rtti_table_header* rtti_enums_;
    const smx_rtti_table_header* rtti_enumstruct_fields_;
    const smx_rtti_table_header* rtti_enumstructs_;
};

} // namespace sp

#endif // _include_sourcepawn_smx_parser_h_
