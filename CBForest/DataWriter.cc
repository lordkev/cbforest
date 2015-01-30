//
//  DataWriter.cc
//  CBForest
//
//  Created by Jens Alfke on 1/26/15.
//  Copyright (c) 2015 Couchbase. All rights reserved.
//

#include "DataWriter.hh"
#include "varint.hh"


namespace forestdb {

    static size_t kMinSharedStringLength = 4, kMaxSharedStringLength = 100;

    dataWriter::dataWriter(std::ostream& out,
                           const std::unordered_map<std::string, uint32_t>* externStrings)
    :_out(out),
     _externStrings(externStrings)
    { }

    void dataWriter::addUVarint(uint64_t n) {
        char buf[kMaxVarintLen64];
        _out.write(buf, PutUVarInt(buf, n));
    }

    void dataWriter::writeNull() {
        addTypeCode(value::kNullCode);
    }

    void dataWriter::writeBool(bool b) {
        addTypeCode(b ? value::kTrueCode : value::kFalseCode);
    }

    void dataWriter::writeInt(int64_t i) {
        char buf[9];
        size_t size;
        memcpy(&buf[1], &i, 8);         //FIX: Endian conversion
        if (i >= INT8_MIN && i <= INT8_MAX) {
            buf[0] = value::kInt8Code;
            size = 2;
        } else if (i >= INT16_MIN && i <= INT16_MAX) {
            buf[0] = value::kInt16Code;
            size = 3;
        } else if (i >= INT32_MIN && i <= INT32_MAX) {
            buf[0] = value::kInt32Code;
            size = 5;
        } else {
            buf[0] = value::kInt64Code;
            size = 9;
        }
        _out.write(buf, size);
    }

    void dataWriter::writeUInt(uint64_t u) {
        if (u < INT64_MAX)
            return writeInt((int64_t)u);
        addTypeCode(value::kUInt64Code);
        _out.write((const char*)&u, 8);         //FIX: Endian conversion
    }

    void dataWriter::writeDouble(double n) {
        if (n == (int64_t)n)
            return writeInt((int64_t)n);
        addTypeCode(value::kFloat64Code);
        _out.write((const char*)&n, 8);         //FIX: Endian conversion
    }

    void dataWriter::writeFloat(float n) {
        if (n == (int32_t)n)
            return writeInt((int32_t)n);
        addTypeCode(value::kFloat32Code);
        _out.write((const char*)&n, 4);         //FIX: Endian conversion
    }

    void dataWriter::writeDate(std::time_t dateTime) {
        addTypeCode(value::kDateCode);
        addUVarint(dateTime);
    }

    void dataWriter::writeData(slice s) {
        addTypeCode(value::kDataCode);
        addUVarint(s.size);
        _out.write((const char*)s.buf, s.size);
    }

    void dataWriter::writeString(slice s) {
        return writeString(std::string(s));
    }

    void dataWriter::writeString(std::string str) {
        if (_externStrings) {
            auto externID = _externStrings->find(str);
            if (externID != _externStrings->end()) {
                // Write reference to extern string:
                addTypeCode(value::kExternStringRefCode);
                addUVarint(externID->second);
                return;
            }
        }

        size_t len = str.length();
        const bool shareable = (len >= kMinSharedStringLength && len <= kMaxSharedStringLength);
        if (shareable) {
            size_t curOffset = _out.tellp();
            size_t sharedOffset = _sharedStrings[str];
            if (sharedOffset > 0) {
                // Change previous string opcode to shared:
                auto pos = _out.tellp();
                _out.seekp(sharedOffset);
                addTypeCode(value::kSharedStringCode);
                _out.seekp(pos);

                // Write reference to previous string:
                addTypeCode(value::kSharedStringRefCode);
                addUVarint(curOffset - sharedOffset);
                return;
            }
            _sharedStrings[str] = curOffset;
        }

        // First appearance, or unshareable, so write the string itself:
        addTypeCode(value::kStringCode);
        addUVarint(len);
        _out << str;
    }

    void dataWriter::beginArray(uint64_t count) {
        addTypeCode(value::kArrayCode);
        addUVarint(count);
    }

    void dataWriter::beginDict(uint64_t count) {
        addTypeCode(value::kDictCode);
        addUVarint(count);
        // Write an empty hash list:
        _savedIndexPos.push_back(_indexPos);
        _indexPos = _out.tellp();
        uint16_t hash = 0;
        for (; count > 0; --count)
            _out.write((char*)&hash, sizeof(hash));
    }

    void dataWriter::writeKey(std::string s) {
        // Go back and write the hash code to the index:
        uint16_t hashCode = dict::hashCode(s);
        uint64_t pos = _out.tellp();
        _out.seekp(_indexPos);
        _out.write((char*)&hashCode, 2);
        _indexPos += 2;
        _out.seekp(pos);

        writeString(s);
    }

    void dataWriter::writeKey(slice str) {
        return writeKey(std::string(str));
    }

    void dataWriter::endDict() {
        _indexPos = _savedIndexPos.back();
        _savedIndexPos.pop_back();
    }

}
