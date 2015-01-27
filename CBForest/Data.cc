//
//  Data.cc
//  CBForest
//
//  Created by Jens Alfke on 1/26/15.
//  Copyright (c) 2015 Couchbase. All rights reserved.
//

#include "Data.hh"
#include "varint.hh"
extern "C" {
#include "murmurhash3_x86_32.h"
}


namespace forestdb {

    static uint8_t kValueTypes[] = {
        kNull,
        kBoolean, kBoolean,
        kNumber, kNumber, kNumber, kNumber, kNumber,
        kNumber, kNumber,
        kNumber,
        kString, kString, kString,
        kData,
        kArray,
        kDict
    };

#pragma mark - VALUE:

    valueType value::type() const {
        return _typeCode < sizeof(kValueTypes) ? (valueType)kValueTypes[_typeCode] : kNull;
    }

    size_t value::getParam() const {
        uint64_t param;
        forestdb::GetUVarInt(slice(_paramStart, 99), &param);
        return param;
    }

    size_t value::getParam(const uint8_t* &after) const {
        uint64_t param;
        after = _paramStart + forestdb::GetUVarInt(slice(_paramStart, 99), &param);
        return param;
    }

    const value* value::next() const {
        const uint8_t* end = _paramStart;
        switch (_typeCode) {
            case kNullCode...kTrueCode:  return (const value*)(end + 0);
            case kInt8Code:              return (const value*)(end + 1);
            case kInt16Code:             return (const value*)(end + 2);
            case kInt32Code:             return (const value*)(end + 4);
            case kInt64Code:             return (const value*)(end + 8);
            default: break;
        }

        uint64_t param = getParam(end);

        switch (_typeCode) {
            case kStringCode:
            case kRawNumberCode:
            case kDataCode:
                end += param;
                break;
            case kSharedStringCode:
            case kExternStringCode:
                break;
            case kArrayCode: {
                // This is somewhat expensive: have to traverse all values in the array
                const value* v = (const value*)end;
                for (; param > 0; --param)
                    v = v->next();
                return v;
            }
            case kDictCode: {
                // This is somewhat expensive: have to traverse all keys+values in the dict
                size_t count;
                const value* key = ((const dict*)this)->firstKey(count);
                for (; count > 0; --count)
                    key = key->next()->next();
                return key;
            }
            default:
                throw "bad typecode";
        }
        return (const value*)end;
    }

    bool value::asBool() const {
        switch (_typeCode) {
            case kNullCode:
            case kFalseCode:
                return false;
                break;
            case kInt8Code...kRawNumberCode:
                return asInt() != 0;
            default:
                return true;
        }
    }

    int64_t value::asInt() const {
        switch (_typeCode) {
            case kNullCode:
            case kFalseCode:
                return 0;
            case kTrueCode:
                return 1;
            case kInt8Code:
                return *(int8_t*)_paramStart;            //TODO: Endian conversions
            case kInt16Code:
                return *(int16_t*)_paramStart;
            case kInt32Code:
                return *(int32_t*)_paramStart;
            case kInt64Code:
                return *(int64_t*)_paramStart;
            case kFloat32Code:
                return (int64_t) *(float*)_paramStart;
            case kFloat64Code:
                return (int64_t) *(double*)_paramStart;
            default:
                throw "value is not a number";
        }
    }

    double value::asDouble() const {
        switch (_typeCode) {
            case kFloat32Code:
                return *(float*)_paramStart;            //TODO: Endian conversions
            case kFloat64Code:
                return *(double*)_paramStart;
            default:
                return (double)asInt();
        }
    }

    slice value::asString() const {
        const uint8_t* payload;
        uint64_t param = getParam(payload);
        switch (_typeCode) {
            case kStringCode:
                return slice(payload, (size_t)param);
            case kSharedStringCode: {
                const value* str = (const value*)offsetby(this, param);
                if (str->_typeCode != kStringCode)
                    throw "invalid shared-string";
                param = str->getParam(payload);
                return slice(payload, (size_t)param);
            }
            case kExternStringCode:
                throw "can't dereference extern string without table";
            default:
                throw "value is not a string";
        }
    }

    uint64_t value::externStringIndex() const {
        if (_typeCode != kExternStringCode)
            throw "value is not extern string";
        return getParam();
    }

    const array* value::asArray() const {
        if (_typeCode != kArrayCode)
            throw "value is not array";
        return (const array*)this;
    }

    const dict* value::asDict() const {
        if (_typeCode != kDictCode)
            throw "value is not dict";
        return (const dict*)this;
    }

#pragma mark - ARRAY:

    const value* array::first() const {
        const uint8_t* f;
        getParam(f);
        return (const value*)f;
    }

#pragma mark - DICT:

    uint16_t dict::hashCode(slice s) {
        uint32_t result;
        MurmurHash3_x86_32(s.buf, (int)s.size, 0, &result);
        return result & 0xFFFF;
    }

    const value* dict::get(forestdb::slice keyToFind, uint16_t hashToFind) const {
        const uint8_t* after;
        size_t count = getParam(after);
        auto hashes = (const uint16_t*)after;

        size_t keyIndex = 0;
        const value* key = (const value*)&hashes[count];
        for (size_t i = 0; i < count; i++) {
            if (hashes[i] == hashToFind) {
                while (keyIndex < i) {
                    key = key->next()->next();
                    ++keyIndex;
                }
                if (keyToFind.compare(key->asString()) == 0)
                    return key->next();
            }
        }
        return NULL;
    }

    const value* dict::firstKey(size_t &count) const {
        const uint8_t* after;
        count = getParam(after);
        return (value*) offsetby(after, count * sizeof(uint16_t));
    }

    dict::iterator::iterator(const dict* d) {
        _key = d->firstKey(_count);
        _value = _count > 0 ? _key->next() : NULL;
    }

    dict::iterator& dict::iterator::operator++() {
        if (_count == 0)
            throw "iterating past end of dict";
        --_count;
        _key = _value->next();
        _value = _key->next();
        return *this;
    }

}