// vim: set sts=2 ts=8 sw=2 tw=99 et:
//
// Copyright (C) 2004-2015 AlliedModers LLC
//
// This file is part of SourcePawn. SourcePawn is licensed under the GNU
// General Public License, version 3.0 (GPL). If a copy of the GPL was not
// provided with this file, you can obtain it here:
//   http://www.gnu.org/licenses/gpl.html
//
#include "smx-v1-image.h"
#include <zlib.h>
#include <fmt/format.h>

using namespace ke;
using namespace sp;

SmxV1Image::SmxV1Image(FILE* fp)
 : FileReader(fp)
 , hdr_(nullptr)
 , header_strings_(nullptr)
 , names_section_(nullptr)
 , names_(nullptr)
 , debug_names_section_(nullptr)
 , debug_names_(nullptr)
 , debug_syms_(nullptr)
 , debug_syms_unpacked_(nullptr) {
}

// Validating SMX v1 scripts is fairly expensive. We reserve real validation
// for v2.
bool
SmxV1Image::validate() {
    if (length_ < sizeof(sp_file_hdr_t))
        return error("bad header");

    hdr_ = (sp_file_hdr_t*)buffer();
    if (hdr_->magic != SmxConsts::FILE_MAGIC)
        return error("bad header");

    switch (hdr_->version) {
        case SmxConsts::SP1_VERSION_1_0:
        case SmxConsts::SP1_VERSION_1_1:
        case SmxConsts::SP1_VERSION_1_7:
            break;
        default:
            return error("unsupported version");
    }

    switch (hdr_->compression) {
        case SmxConsts::FILE_COMPRESSION_GZ: {
            // We don't support junk in binaries, check that disksize matches the actual file size.
            // (this is to avoid a known crash in inflate() if told that data is bigger than it is)
            if (hdr_->disksize > length_)
                return error("illegal disk size");

            // The start of the compression cannot be larger than the file.
            if (hdr_->dataoffs > length_)
                return error("illegal compressed region");

            // The compressed region must start after the header.
            if (hdr_->dataoffs < sizeof(sp_file_hdr_t))
                return error("illegal compressed region");

            // The full size of the image must be at least as large as the start
            // of the compressed region.
            if (hdr_->imagesize < hdr_->dataoffs)
                return error("illegal image size");

            // Allocate the uncompressed image buffer.
            uint32_t compressedSize = hdr_->disksize - hdr_->dataoffs;
            std::unique_ptr<uint8_t[]> uncompressed = std::make_unique<uint8_t[]>(hdr_->imagesize);
            if (!uncompressed)
                return error("out of memory");

            // Decompress.
            const uint8_t* src = buffer() + hdr_->dataoffs;
            uint8_t* dest = uncompressed.get() + hdr_->dataoffs;
            uLongf destlen = hdr_->imagesize - hdr_->dataoffs;
            int rv = uncompress((Bytef*)dest, &destlen, src, compressedSize);
            if (rv != Z_OK)
                return error("could not decode compressed region");

            // Copy the initial uncompressed region back in.
            memcpy(uncompressed.get(), buffer(), hdr_->dataoffs);

            // Replace the original buffer.
            length_ = hdr_->imagesize;
            buffer_ = std::move(uncompressed);
            hdr_ = (sp_file_hdr_t*)buffer();
            break;
        }

        case SmxConsts::FILE_COMPRESSION_NONE:
            break;

        default:
            return error("unknown compression type");
    }

    // Validate the string table.
    if (hdr_->stringtab >= length_)
        return error("invalid string table");
    header_strings_ = reinterpret_cast<const char*>(buffer() + hdr_->stringtab);

    // Validate sections header.
    if ((sizeof(sp_file_hdr_t) + hdr_->sections * sizeof(sp_file_section_t)) > length_)
        return error("invalid section table");

    size_t last_header_string = 0;
    const sp_file_section_t* sections =
        reinterpret_cast<const sp_file_section_t*>(buffer() + sizeof(sp_file_hdr_t));
    for (size_t i = 0; i < hdr_->sections; i++) {
        if (sections[i].nameoffs >= (hdr_->dataoffs - hdr_->stringtab))
            return error("invalid section name");

        if (sections[i].nameoffs > last_header_string)
            last_header_string = sections[i].nameoffs;

        sections_.push_back(Section());
        sections_.back().dataoffs = sections[i].dataoffs;
        sections_.back().size = sections[i].size;
        sections_.back().name = header_strings_ + sections[i].nameoffs;
    }

    // Validate sanity of section header strings.
    bool found_terminator = false;
    for (const uint8_t* iter = buffer() + last_header_string; iter < buffer() + hdr_->dataoffs;
         iter++) {
        if (*iter == '\0') {
            found_terminator = true;
            break;
        }
    }
    if (!found_terminator)
        return error("malformed section names header");

    names_section_ = findSection(".names");
    if (!names_section_)
        return error("could not find .names section");
    if (!validateSection(names_section_))
        return error("invalid names section");
    names_ = reinterpret_cast<const char*>(buffer() + names_section_->dataoffs);

    // The names section must be 0-length or be null-terminated.
    if (names_section_->size != 0 && *(names_ + names_section_->size - 1) != '\0') {
        return error("malformed names section");
    }

    if (!validateCode())
        return false;
    if (!validateData())
        return false;
    if (!validatePublics())
        return false;
    if (!validatePubvars())
        return false;
    if (!validateNatives())
        return false;
    if (!validateRtti())
        return false;
    if (!validateDebugInfo())
        return false;
    if (!validateTags())
        return false;

    return true;
}

const SmxV1Image::Section*
SmxV1Image::findSection(const char* name) {
    for (size_t i = 0; i < sections_.size(); i++) {
        if (strcmp(sections_[i].name, name) == 0)
            return &sections_[i];
    }
    return nullptr;
}

bool
SmxV1Image::validateSection(const Section* section) {
    if (section->dataoffs >= length_)
        return false;
    if (section->size > length_ - section->dataoffs)
        return false;
    return true;
}

bool
SmxV1Image::validateData() {
    // .data is required.
    const Section* section = findSection(".data");
    if (!section)
        return error("could not find data");
    if (!validateSection(section))
        return error("invalid data section");

    const sp_file_data_t* data =
        reinterpret_cast<const sp_file_data_t*>(buffer() + section->dataoffs);
    if (data->data > section->size)
        return error("invalid data blob");
    if (data->datasize > (section->size - data->data))
        return error("invalid data blob");

    const uint8_t* blob = reinterpret_cast<const uint8_t*>(data) + data->data;
    data_ = Blob<sp_file_data_t>(section, data, blob, data->datasize, 0);
    return true;
}

bool
SmxV1Image::validateCode() {
    // .code is required.
    const Section* section = findSection(".code");
    if (!section)
        return error("could not find code");
    if (!validateSection(section))
        return error("invalid code section");

    const sp_file_code_t* code =
        reinterpret_cast<const sp_file_code_t*>(buffer() + section->dataoffs);
    if (code->codeversion < SmxConsts::CODE_VERSION_MINIMUM)
        return error("code version is too old, no longer supported");
    if (code->codeversion > SmxConsts::CODE_VERSION_CURRENT)
        return error("code version is too new, not supported");
    if (code->cellsize != 4)
        return error("unsupported cellsize");
    if (code->flags & ~CODEFLAG_DEBUG)
        return error("unsupported code settings");
    if (code->code > section->size)
        return error("invalid code blob");
    if (code->codesize > (section->size - code->code))
        return error("invalid code blob");

    uint32_t features = 0;
    if (code->codeversion >= SmxConsts::CODE_VERSION_FEATURE_MASK)
        features = code->features;

    uint32_t supported_features = SmxConsts::kCodeFeatureDirectArrays;
    if (features & ~supported_features)
        return error("unsupported feature set; code is too new");

    const uint8_t* blob = reinterpret_cast<const uint8_t*>(code) + code->code;
    code_ = Blob<sp_file_code_t>(section, code, blob, code->codesize, features);
    return true;
}

bool
SmxV1Image::validatePublics() {
    const Section* section = findSection(".publics");
    if (!section)
        return true;
    if (!validateSection(section))
        return error("invalid .publics section");
    if ((section->size % sizeof(sp_file_publics_t)) != 0)
        return error("invalid .publics section");

    const sp_file_publics_t* publics =
        reinterpret_cast<const sp_file_publics_t*>(buffer() + section->dataoffs);
    size_t length = section->size / sizeof(sp_file_publics_t);

    for (size_t i = 0; i < length; i++) {
        if (!validateName(publics[i].name))
            return error("invalid public name");
    }

    publics_ = List<sp_file_publics_t>(publics, length);
    return true;
}

bool
SmxV1Image::validatePubvars() {
    const Section* section = findSection(".pubvars");
    if (!section)
        return true;
    if (!validateSection(section))
        return error("invalid .pubvars section");
    if ((section->size % sizeof(sp_file_pubvars_t)) != 0)
        return error("invalid .pubvars section");

    const sp_file_pubvars_t* pubvars =
        reinterpret_cast<const sp_file_pubvars_t*>(buffer() + section->dataoffs);
    size_t length = section->size / sizeof(sp_file_pubvars_t);

    for (size_t i = 0; i < length; i++) {
        if (!validateName(pubvars[i].name))
            return error("invalid pubvar name");
    }

    pubvars_ = List<sp_file_pubvars_t>(pubvars, length);
    return true;
}

bool
SmxV1Image::validateNatives() {
    const Section* section = findSection(".natives");
    if (!section)
        return true;
    if (!validateSection(section))
        return error("invalid .natives section");
    if ((section->size % sizeof(sp_file_natives_t)) != 0)
        return error("invalid .natives section");

    const sp_file_natives_t* natives =
        reinterpret_cast<const sp_file_natives_t*>(buffer() + section->dataoffs);
    size_t length = section->size / sizeof(sp_file_natives_t);

    for (size_t i = 0; i < length; i++) {
        if (!validateName(natives[i].name))
            return error("invalid pubvar name");
    }

    natives_ = List<sp_file_natives_t>(natives, length);
    return true;
}

bool
SmxV1Image::validateName(size_t offset) {
    return offset < names_section_->size;
}

bool
SmxV1Image::validateRtti() {
    const Section* rtti_data = findSection("rtti.data");
    if (!rtti_data)
        return true;
    if (!validateSection(rtti_data))
        return error("invalid rtti.data section");

    const uint8_t* blob = reinterpret_cast<const uint8_t*>(buffer() + rtti_data->dataoffs);
    rtti_data_ = std::make_unique<const debug::RttiData>(blob, rtti_data->size);
    
    const char* tables[] = {
        "rtti.methods",
        "rtti.natives",
    };
    for (size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
        const char* table_name = tables[i];
        const Section* section = findSection(table_name);
        if (!section) {
            error_ = fmt::format("missing %s section", table_name);
            return false;
        }
        if (!validateRttiHeader(section)) {
            error_ = fmt::format("could not validate %s section", table_name);
            return false;
        }
    }

    rtti_enums_ = findRttiSection("rtti.enums");
    if (rtti_enums_ && !validateRttiEnums())
        return false;

    rtti_enumstruct_fields_ = findRttiSection("rtti.enumstruct_fields");
    rtti_enumstructs_ = findRttiSection("rtti.enumstructs");
    if (rtti_enumstructs_ && !validateRttiEnumStructs())
        return false;


    rtti_methods_ = findRttiSection("rtti.methods");
    if (rtti_methods_ && !validateRttiMethods())
        return false;

    rtti_fields_ = findRttiSection("rtti.fields");
    rtti_classdefs_ = findRttiSection("rtti.classdefs");
    if (rtti_classdefs_ && !validateRttiClassdefs())
        return false;

    return true;
}

bool
SmxV1Image::validateRttiMethods() {
    for (uint32_t i = 0; i < rtti_methods_->row_count; i++) {
        const smx_rtti_method* method = getRttiRow<smx_rtti_method>(rtti_methods_, i);
        if (!validateName(method->name))
            return error("invalid method name");
        if (method->signature >= rtti_data_->size())
            return error("invalid method signature type offset");
        if (method->pcode_start > method->pcode_end)
            return error("invalid method code range");
        if (method->pcode_start >= code_.header()->size)
            return error("invalid method code start");
        if (method->pcode_end > code_.header()->size)
            return error("invalid method code end");
    }
    return true;
}

bool
SmxV1Image::validateRttiHeader(const Section* section) {
    if (!validateSection(section))
        return false;

    const smx_rtti_table_header* header =
        reinterpret_cast<const smx_rtti_table_header*>(buffer() + section->dataoffs);
    if (section->size < sizeof(smx_rtti_table_header))
        return false;
    if (section->size < header->header_size)
        return false;
    if (!IsUint32MultiplySafe(header->row_size, header->row_count))
        return false;

    uint32_t table_size = header->row_size * header->row_count;
    if (!IsUint32AddSafe(table_size, header->header_size))
        return false;
    if (section->size != header->header_size + table_size)
        return false;
    return true;
}

bool
SmxV1Image::validateDebugInfo() {
    const Section* dbginfo = findSection(".dbg.info");
    if (!dbginfo)
        return true;
    if (!validateSection(dbginfo))
        return error("invalid .dbg.info section");

    debug_info_ = reinterpret_cast<const sp_fdbg_info_t*>(buffer() + dbginfo->dataoffs);

    // Pre-RTTI, the debug tables used a separate string table. That is no longer
    // the case, but we support both scenarios.
    debug_names_section_ = findSection(".dbg.strings");
    if (debug_names_section_) {
        if (!validateSection(debug_names_section_))
            return error("invalid .dbg.strings section");
        debug_names_ = reinterpret_cast<const char*>(buffer() + debug_names_section_->dataoffs);

        // Name tables must be null-terminated.
        if (debug_names_section_->size != 0 &&
            *(debug_names_ + debug_names_section_->size - 1) != '\0') {
            return error("invalid .dbg.strings section");
        }
    } else {
        debug_names_section_ = names_section_;
        debug_names_ = names_;
    }

    const Section* files = findSection(".dbg.files");
    if (!files)
        return error("no debug file table");
    if (!validateSection(files))
        return error("invalid debug file table");
    if (files->size < sizeof(sp_fdbg_file_t) * debug_info_->num_files)
        return error("invalid debug file table");
    debug_files_ =
        List<sp_fdbg_file_t>(reinterpret_cast<const sp_fdbg_file_t*>(buffer() + files->dataoffs),
                             debug_info_->num_files);

    const Section* lines = findSection(".dbg.lines");
    if (!lines)
        return error("no debug lines table");
    if (!validateSection(lines))
        return error("invalid debug lines table");
    if (lines->size < sizeof(sp_fdbg_line_t) * debug_info_->num_lines)
        return error("invalid debug lines table");
    debug_lines_ =
        List<sp_fdbg_line_t>(reinterpret_cast<const sp_fdbg_line_t*>(buffer() + lines->dataoffs),
                             debug_info_->num_lines);

    debug_symbols_section_ = findSection(".dbg.symbols");
    if (debug_symbols_section_) {
        if (!validateSection(debug_symbols_section_))
            return error("invalid debug symbol table");
    } else {
        // New debug symbol tables are optional, but if present, they need to be
        // coherent.
        if (globals_ = findRttiSection(".dbg.globals")) {
        }
        if (locals_ = findRttiSection(".dbg.locals")) {
        }
        if (methods_ = findRttiSection(".dbg.methods")) {
        }
    }

    if (debug_symbols_section_) {
        // See the note about unpacked debug sections in smx-headers.h.
        if (hdr_->version == SmxConsts::SP1_VERSION_1_0 && !findSection(".dbg.natives")) {
            debug_syms_unpacked_ = reinterpret_cast<const sp_u_fdbg_symbol_t*>(
                buffer() + debug_symbols_section_->dataoffs);
        } else {
            debug_syms_ = reinterpret_cast<const sp_fdbg_symbol_t*>(
                buffer() + debug_symbols_section_->dataoffs);
        }
    }

    return true;
}

bool
SmxV1Image::validateTags() {
    const Section* section = findSection(".tags");
    if (!section)
        return true;
    if (!validateSection(section))
        return error("invalid .tags section");
    if ((section->size % sizeof(sp_file_tag_t)) != 0)
        return error("invalid .tags section");

    const sp_file_tag_t* tags =
        reinterpret_cast<const sp_file_tag_t*>(buffer() + section->dataoffs);
    size_t length = section->size / sizeof(sp_file_tag_t);

    for (size_t i = 0; i < length; i++) {
        if (!validateName(tags[i].name))
            return error("invalid tag name");
    }

    tags_ = List<sp_file_tag_t>(tags, length);
    return true;
}

SmxV1Image::SymbolIterator
SmxV1Image::symboliterator(bool global) {
    if (debug_syms_) {
        SymbolIterator iter((uint8_t*)debug_syms_, debug_symbols_section_->size, 1, this);
        return iter;
    }
    if (debug_syms_unpacked_) {
        SymbolIterator iter((uint8_t*)debug_syms_unpacked_, debug_symbols_section_->size, 0, this);
        return iter;
    }
    if (!global) {
        const smx_rtti_debug_var* variable = getRttiRow<smx_rtti_debug_var>(locals_, 0);
        SymbolIterator iter((uint8_t*)variable, locals_->row_size * sizeof(smx_rtti_debug_var), 2,
                            this);
        return iter;
    } else {
        const smx_rtti_debug_var* variable = getRttiRow<smx_rtti_debug_var>(globals_, 0);
        SymbolIterator iter((uint8_t*)variable, globals_->row_size * sizeof(smx_rtti_debug_var), 3,
                            this);
        return iter;        
    }
}

auto
SmxV1Image::DescribeCode() const -> LegacyImage::Code {
    LegacyImage::Code code;
    code.bytes = code_.blob();
    code.length = code_.length();
    code.version = code_->codeversion;
    return code;
}

auto
SmxV1Image::DescribeData() const -> LegacyImage::Data {
    LegacyImage::Data data;
    data.bytes = data_.blob();
    data.length = data_.length();
    return data;
}

size_t
SmxV1Image::NumNatives() const {
    return natives_.length();
}

const char*
SmxV1Image::GetNative(size_t index) const {
    assert(index < natives_.length());
    return names_ + natives_[index].name;
}

bool
SmxV1Image::FindNative(const char* name, size_t* indexp) const {
    for (size_t i = 0; i < natives_.length(); i++) {
        const char* candidate = names_ + natives_[i].name;
        if (strcmp(candidate, name) == 0) {
            if (indexp)
                *indexp = i;
            return true;
        }
    }
    return false;
}

size_t
SmxV1Image::NumPublics() const {
    return publics_.length();
}

void
SmxV1Image::GetPublic(size_t index, uint32_t* offsetp, const char** namep) const {
    assert(index < publics_.length());
    if (offsetp)
        *offsetp = publics_[index].address;
    if (namep)
        *namep = names_ + publics_[index].name;
}

bool
SmxV1Image::FindPublic(const char* name, size_t* indexp) const {
    int high = publics_.length() - 1;
    int low = 0;
    while (low <= high) {
        int mid = (low + high) / 2;
        const char* candidate = names_ + publics_[mid].name;
        int diff = strcmp(candidate, name);
        if (diff == 0) {
            if (indexp) {
                *indexp = mid;
                return true;
            }
        }
        if (diff < 0)
            low = mid + 1;
        else
            high = mid - 1;
    }
    return false;
}

size_t
SmxV1Image::NumPubvars() const {
    return pubvars_.length();
}

void
SmxV1Image::GetPubvar(size_t index, uint32_t* offsetp, const char** namep) const {
    assert(index < pubvars_.length());
    if (offsetp)
        *offsetp = pubvars_[index].address;
    if (namep)
        *namep = names_ + pubvars_[index].name;
}

bool
SmxV1Image::FindPubvar(const char* name, size_t* indexp) const {
    int high = pubvars_.length() - 1;
    int low = 0;
    while (low <= high) {
        int mid = (low + high) / 2;
        const char* candidate = names_ + pubvars_[mid].name;
        int diff = strcmp(candidate, name);
        if (diff == 0) {
            if (indexp) {
                *indexp = mid;
                return true;
            }
        }
        if (diff < 0)
            low = mid + 1;
        else
            high = mid - 1;
    }
    return false;
}

size_t
SmxV1Image::HeapSize() const {
    return data_->memsize;
}

size_t
SmxV1Image::ImageSize() const {
    return length_;
}

const char*
SmxV1Image::LookupFile(uint32_t addr) {
    int high = debug_files_.length();
    int low = -1;

    while (high - low > 1) {
        int mid = (low + high) / 2;
        if (debug_files_[mid].addr <= addr)
            low = mid;
        else
            high = mid;
    }

    if (low == -1)
        return nullptr;
    if (debug_files_[low].name >= debug_names_section_->size)
        return nullptr;

    return debug_names_ + debug_files_[low].name;
}

template <typename SymbolType, typename DimType>
const char*
SmxV1Image::lookupFunction(const SymbolType* syms, uint32_t addr) {
    const uint8_t* cursor = reinterpret_cast<const uint8_t*>(syms);
    const uint8_t* cursor_end = cursor + debug_symbols_section_->size;
    for (uint32_t i = 0; i < debug_info_->num_syms; i++) {
        if (cursor + sizeof(SymbolType) > cursor_end)
            break;

        const SymbolType* sym = reinterpret_cast<const SymbolType*>(cursor);
        if (sym->ident == sp::IDENT_FUNCTION && sym->codestart <= addr && sym->codeend > addr) {
            if (sym->name >= debug_names_section_->size)
                return nullptr;
            return debug_names_ + sym->name;
        }

        if (sym->dimcount > 0)
            cursor += sizeof(DimType) * sym->dimcount;
        cursor += sizeof(SymbolType);
    }
    return nullptr;
}

const char*
SmxV1Image::LookupFunction(uint32_t code_offset) {
    if (debug_syms_) {
        return lookupFunction<sp_fdbg_symbol_t, sp_fdbg_arraydim_t>(debug_syms_, code_offset);
    }
    return lookupFunction<sp_u_fdbg_symbol_t, sp_u_fdbg_arraydim_t>(debug_syms_unpacked_,
                                                                    code_offset);
}

bool
SmxV1Image::LookupLine(uint32_t addr, uint32_t* line) {
    int high = debug_lines_.length();
    int low = -1;

    while (high - low > 1) {
        int mid = (low + high) / 2;
        if (debug_lines_[mid].addr <= addr)
            low = mid;
        else
            high = mid;
    }

    if (low == -1)
        return false;

    // Since the CIP occurs BEFORE the line, we have to add one.
    *line = debug_lines_[low].line + 1;
    return true;
}

template <typename SymbolType, typename DimType>
bool
SmxV1Image::getFunctionAddress(const SymbolType* syms, const char* name, uint32_t* addr,
                               uint32_t* index) {
    unsigned int i = 0;
    SymbolIterator iter = symboliterator(false);
    while (!iter.Done()) {
        Symbol sym = iter.Next();

        if (i >= *index && sym.ident() == sp::IDENT_FUNCTION &&
            sym.name() < debug_names_section_->size && !strcmp(debug_names_ + sym.name(), name)) {
            *addr = sym.addr();
            return true;
        }

        if (i == *index)
            (*index)++;
        i++;
    }
    return false;
}

bool
SmxV1Image::GetFunctionAddress(const char* function, const char* file, uint32_t* funcaddr) {
    uint32_t index = 0;
    const char* tgtfile;
    *funcaddr = 0;
    for (;;) {
        // find (next) matching function
        if (debug_syms_) {
            getFunctionAddress<sp_fdbg_symbol_t, sp_fdbg_arraydim_t>(debug_syms_, function,
                                                                     funcaddr, &index);
        } else {
            getFunctionAddress<sp_u_fdbg_symbol_t, sp_u_fdbg_arraydim_t>(
                debug_syms_unpacked_, function, funcaddr, &index);
        }

        if (index >= debug_info_->num_syms)
            return false;

        // verify that this function is defined in the apprpriate file
        tgtfile = LookupFile(*funcaddr);
        if (tgtfile != nullptr && strcmp(file, tgtfile) == 0)
            break;
        index++;
    }
    assert(index < debug_info_->num_syms);

    // now find the first line in the function where we can "break" on
    for (index = 0; index < debug_info_->num_lines && debug_lines_[index].addr < *funcaddr; index++)
        /* nothing */;

    if (index >= debug_info_->num_lines)
        return false;

    *funcaddr = debug_lines_[index].addr;
    return true;
}

bool
SmxV1Image::GetLineAddress(const uint32_t line, const char* filename, uint32_t* addr) {
    /* Find a suitable "breakpoint address" close to the indicated line (and in
   * the specified file). The address is moved up to the next "breakable" line
   * if no "breakpoint" is available on the specified line. You can use function
   * LookupLine() to find out at which precise line the breakpoint was set.
   *
   * The filename comparison is strict (case sensitive and path sensitive).
   */
    *addr = 0;

    uint32_t bottomaddr, topaddr;
    uint32_t file;
    uint32_t index = 0;
    for (file = 0; file < debug_info_->num_files; file++) {
        // find the (next) matching instance of the file
        if (debug_files_[file].name >= debug_names_section_->size ||
            strcmp(debug_names_ + debug_files_[file].name, filename) != 0) {
            continue;
        }

        // get address range for the current file
        bottomaddr = debug_files_[file].addr;
        topaddr = (file + 1 < debug_info_->num_files) ? debug_files_[file + 1].addr : (uint32_t)-1;

        // go to the starting address in the line table
        while (index < debug_info_->num_lines && debug_lines_[index].addr < bottomaddr)
            index++;

        // browse until the line is found or until the top address is exceeded
        while (index < debug_info_->num_lines && debug_lines_[index].line < line &&
               debug_lines_[index].addr < topaddr) {
            index++;
        }

        if (index >= debug_info_->num_lines)
            return false;
        if (debug_lines_[index].line >= line)
            break;

        // if not found (and the line table is not yet exceeded) try the next
        // instance of the same file (a file may appear twice in the file table)
    }
    if (file >= debug_info_->num_files)
        return false;

    assert(index < debug_info_->num_lines);
    *addr = debug_lines_[index].addr;
    return true;
}

const char*
SmxV1Image::FindFileByPartialName(const char* partialname) {
    // the user may have given a partial filename (e.g. without a path), so
    // walk through all files to find a match.
    int len = strlen(partialname);
    int offs;
    const char* filename;
    for (uint32_t i = 0; i < debug_info_->num_files; i++) {
        // Invalid name offset?
        if (debug_files_[i].name >= debug_names_section_->size)
            continue;

        filename = debug_names_ + debug_files_[i].name;
        offs = strlen(filename) - len;
        if (offs >= 0 && !strncmp(filename + offs, partialname, len)) {
            return filename;
        }
    }
    return nullptr;
}

const char*
SmxV1Image::GetTagName(uint32_t tag) {
    unsigned int index;
    for (index = 0; index < tags_.length() && tags_[index].tag_id != tag; index++)
        /* nothing */;
    if (index >= tags_.length())
        return nullptr;

    return names_ + tags_[index].name;
}

bool
SmxV1Image::GetVariable(const char* symname, uint32_t scopeaddr, std::unique_ptr<Symbol>& sym) {
    uint32_t codestart = 0;
    uint32_t codeend = 0;

    sym = nullptr;

    SymbolIterator iter = symboliterator(false);
    while (!iter.Done()) {
        // find (next) matching variable
        sym = std::make_unique<Symbol>(iter.Next());

        if (sym->codestart() <= scopeaddr && sym->codeend() >= scopeaddr &&              
            strcmp(debug_names_ + sym->name(), symname) == 0) {            
            break;
        }
        sym = nullptr;
    }
    if (sym == nullptr) {
        iter = symboliterator(true);
        while (!iter.Done()) {
            // find (next) matching variable
            sym = std::make_unique<Symbol>(iter.Next());

            if (
                strcmp(debug_names_ + sym->name(), symname) == 0) {
                break;
            }
            sym = nullptr;
        }

    }
    return sym != nullptr;
}

const char*
SmxV1Image::GetDebugName(uint32_t nameoffs) {
    if (nameoffs >= debug_names_section_->size)
        return nullptr;
    return debug_names_ + nameoffs;
}

const char*
SmxV1Image::GetFileName(uint32_t index) {
    if (debug_files_[index].name >= debug_names_section_->size)
        return nullptr;
    return debug_names_ + debug_files_[index].name;
}

uint32_t
SmxV1Image::GetFileCount() {
    return debug_info_->num_files;
}

std::vector<SmxV1Image::ArrayDim*>*
SmxV1Image::GetArrayDimensions(const Symbol* sym) {
    if (sym->ident() != sp::IDENT_ARRAY && sym->ident() != IDENT_REFARRAY)
        return nullptr;

    assert(sym->dimcount() > 0); // array must have at least one dimension

    // find he end of the symbol name
    const char* ptr = (const char*)sym->sym();
    auto type = sym->type();
    if (type == Symbol::VAR_PACKED) {
        ptr += sizeof(sp_fdbg_symbol_t);
    } else if (type == Symbol::VAR_UNPACKED) {
        ptr += sizeof(sp_u_fdbg_symbol_t);
    }
    if (type != Symbol::VAR_RTTI) {
        std::vector<ArrayDim*>* dims = new std::vector<ArrayDim*>();
        for (int i = 0; i < sym->dimcount(); i++) {
            if (sym->packed()) {
                dims->push_back(new ArrayDim((sp_fdbg_arraydim_t*)ptr));
                ptr += sizeof(sp_fdbg_arraydim_t);
            } else {
                // There's a padding of 2 bytes before this short.
                ptr += 2;
                dims->push_back(new ArrayDim((sp_u_fdbg_arraydim_t*)ptr));
                ptr += sizeof(sp_u_fdbg_arraydim_t);
            }
        }
        return dims;
    } else {
        std::vector<ArrayDim*>* dims = new std::vector<ArrayDim*>();
        auto sym = (smx_rtti_debug_var*)ptr;
        int kind = (sym->type_id) & 0xf;
        int payload = ((sym->type_id) >> 4) & 0xfffffff;
        auto DecodeUint32 = [](unsigned char* bytes, int &offset) {
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
        Decode = [dims, DecodeUint32, &Decode](unsigned char* bytes, int& offset) {
            unsigned char b = bytes[offset++];
            switch (b) {
                case cb::kFixedArray: {
                    auto dimcount_ = DecodeUint32(bytes, offset);
                    dims->push_back(new ArrayDim(dimcount_));
                    Decode(bytes, offset);
                    break;
                }
            }
        };
        if (kind == kTypeId_Inline) {
            unsigned char temp[4];
            temp[0] = (payload & 0xff);
            temp[1] = ((payload >> 8) & 0xff);
            temp[2] = ((payload >> 16) & 0xff);
            temp[3] = ((payload >> 24) & 0xff);
            int offset = 0;
            Decode(temp, offset);
        }
        return dims;
    }
    
}


bool
SmxV1Image::validateRttiField(uint32_t index) {
    if (index >= rtti_fields_->row_count)
        return error("invalid classdef field index");

    // TODO: Validate flags.
    const smx_rtti_field* field = getRttiRow<smx_rtti_field>(rtti_fields_, index);
    if (!validateName(field->name))
        return error("invalid classdef field name");
    if (!rtti_data_->validateType(field->type_id))
        return error("invalid classdef field type");
    return true;
}
size_t SmxV1Image::getTypeFromTypeId(uint32_t typeId) {
    int kind = (typeId)&0xf;
    int payload = ((typeId) >> 4) & 0xfffffff;
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

    if (kind == kTypeId_Inline) {
        unsigned char temp[4];
        temp[0] = (payload & 0xff);
        temp[1] = ((payload >> 8) & 0xff);
        temp[2] = ((payload >> 16) & 0xff);
        temp[3] = ((payload >> 24) & 0xff);
        int offset = 0;
        char b = temp[offset++];
        if (b == cb::kConst) {
            b = temp[offset++];
        }
        return b;
    }
    if (kind != kTypeId_Complex) {
        //todo
    }
    return 0;
};
std::vector<smx_rtti_es_field*>
SmxV1Image::getEnumFields(uint32_t index) {
    const smx_rtti_enumstruct* enumstruct = getRttiRow<smx_rtti_enumstruct>(rtti_enumstructs_, index);
    // Calculate how many fields this class has.
    uint32_t stopat = rtti_enumstruct_fields_->row_count;
    if (index != rtti_enumstructs_->row_count - 1) {
        const smx_rtti_enumstruct* next_enumstruct =
            getRttiRow<smx_rtti_enumstruct>(rtti_enumstructs_, index + 1);
        stopat = next_enumstruct->first_field;
    }
    if (enumstruct->first_field >= stopat)
        return {};

    std::vector<smx_rtti_es_field*> ret;
    for (uint32_t j = enumstruct->first_field; j < stopat; j++) {
        if (!validateRttiEnumStructField(enumstruct, j))
            return {};
        const smx_rtti_es_field* field = getRttiRow<smx_rtti_es_field>(rtti_enumstruct_fields_, j);
        ret.push_back(const_cast<smx_rtti_es_field*>(field));
    }
    return ret;
}


std::vector<smx_rtti_field*>
SmxV1Image::getTypeFields(uint32_t index) {
    const smx_rtti_classdef* classdef = getRttiRow<smx_rtti_classdef>(rtti_classdefs_, index);
    // Calculate how many fields this class has.
    uint32_t stopat = rtti_fields_->row_count;
    if (index != rtti_classdefs_->row_count - 1) {
        const smx_rtti_classdef* next_classdef =
            getRttiRow<smx_rtti_classdef>(rtti_classdefs_, index + 1);
        stopat = next_classdef->first_field;
    }
    if (classdef->first_field >= stopat)
        return {};

    std::vector<smx_rtti_field*> ret;
    for (uint32_t j = classdef->first_field; j < stopat; j++) {
        if (!validateRttiField(j))
            return {};
        const smx_rtti_field* field = getRttiRow<smx_rtti_field>(rtti_fields_, j);
        ret.push_back(const_cast<smx_rtti_field*>(field));
    }
    return ret;
}

bool
SmxV1Image::validateRttiClassdefs() {
    if (!rtti_fields_)
        return error("rtti.fields section missing");

    for (uint32_t i = 0; i < rtti_classdefs_->row_count; i++) {
        const smx_rtti_classdef* classdef = getRttiRow<smx_rtti_classdef>(rtti_classdefs_, i);
        // TODO: Validate flags.
        if (!validateName(classdef->name))
            return error("invalid classdef name");

        // Calculate how many fields this class has.
        uint32_t stopat = rtti_fields_->row_count;
        if (i != rtti_classdefs_->row_count - 1) {
            const smx_rtti_classdef* next_classdef =
                getRttiRow<smx_rtti_classdef>(rtti_classdefs_, i + 1);
            stopat = next_classdef->first_field;
        }
        if (classdef->first_field >= stopat)
            return error("invalid classdef fields boundary");

        for (uint32_t j = classdef->first_field; j < stopat; j++) {
            if (!validateRttiField(j))
                return false;
        }
    }
    return true;
}


bool
SmxV1Image::validateRttiEnums() {
    for (uint32_t i = 0; i < rtti_enums_->row_count; i++) {
        const smx_rtti_enum* enumType = getRttiRow<smx_rtti_enum>(rtti_enums_, i);
        if (!validateName(enumType->name))
            return error("invalid enum name");
    }
    return true;
}

bool
SmxV1Image::validateRttiEnumStructField(const smx_rtti_enumstruct* enumstruct, uint32_t index) {
    if (index >= rtti_enumstruct_fields_->row_count)
        return error("invalid enum struct field index");

    const smx_rtti_es_field* field = getRttiRow<smx_rtti_es_field>(rtti_enumstruct_fields_, index);
    if (!validateName(field->name))
        return error("invalid enum struct field name");
    if (field->offset >= enumstruct->size * 4)
        return error("invalid enum struct field offset");
    if (!rtti_data_->validateType(field->type_id))
        return error("invalid enum struct field type");
    return true;
}

bool
SmxV1Image::validateRttiEnumStructs() {
    if (!rtti_enumstruct_fields_)
        return error("rtti.enumstruct_fields section missing");

    for (uint32_t i = 0; i < rtti_enumstructs_->row_count; i++) {
        const smx_rtti_enumstruct* enumstruct =
            getRttiRow<smx_rtti_enumstruct>(rtti_enumstructs_, i);
        if (!validateName(enumstruct->name))
            return error("invalid enum struct name");

        // Calculate how many fields this enumstruct has.
        uint32_t stopat = rtti_enumstruct_fields_->row_count;
        if (i != rtti_enumstructs_->row_count - 1) {
            const smx_rtti_enumstruct* next_enumstruct =
                getRttiRow<smx_rtti_enumstruct>(rtti_enumstructs_, i + 1);
            stopat = next_enumstruct->first_field;
        }
        if (enumstruct->first_field >= stopat)
            return error("invalid enum struct fields boundary");

        for (uint32_t j = enumstruct->first_field; j < stopat; j++) {
            if (!validateRttiEnumStructField(enumstruct, j))
                return false;
        }
    }
    return true;
}