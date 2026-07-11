#include "piv_objects.h"
#include "piv_tlv.h"

#include <cstring>

namespace cdc::mod_piv {

// Full 11-byte PIV AID (RID A0 00 00 03 08 + app 00 00 10 00 + version 01 00).
static const uint8_t kPivAidFull[11] = {
    0xA0, 0x00, 0x00, 0x03, 0x08, 0x00, 0x00, 0x10, 0x00, 0x01, 0x00
};
// 5-byte NIST RID reported inside the allocation-authority template.
static const uint8_t kNistRid[5] = {0xA0, 0x00, 0x00, 0x03, 0x08};

uint8_t certObjectToKeyRef(uint32_t objectTag) {
    switch (objectTag) {
        case PIV_OBJ_CERT_9A: return PIV_KEY_9A;
        case PIV_OBJ_CERT_9C: return PIV_KEY_9C;
        case PIV_OBJ_CERT_9D: return PIV_KEY_9D;
        case PIV_OBJ_CERT_9E: return PIV_KEY_9E;
        default: return 0;
    }
}

size_t buildApt(uint8_t* out, size_t outCap) {
    // Inner content of tag 0x61.
    uint8_t inner[64];
    size_t ip = 0;

    // 4F: application identifier (full AID).
    if (!tlvWrite(inner, sizeof(inner), &ip, PIV_TAG_AID, kPivAidFull, sizeof(kPivAidFull)))
        return 0;

    // 79 { 4F <RID> }: coexistent tag allocation authority.
    uint8_t alloc[16];
    size_t ap = 0;
    if (!tlvWrite(alloc, sizeof(alloc), &ap, PIV_TAG_AID, kNistRid, sizeof(kNistRid)))
        return 0;
    if (!tlvWrite(inner, sizeof(inner), &ip, PIV_TAG_ALLOC_AUTH, alloc, ap))
        return 0;

    // 50: application label.
    static const char kLabel[] = "CDC Badge PIV";
    if (!tlvWrite(inner, sizeof(inner), &ip, PIV_TAG_APP_LABEL,
                  reinterpret_cast<const uint8_t*>(kLabel), sizeof(kLabel) - 1))
        return 0;

    // AC { 80 01 11, 06 01 00 }: supported algorithm(s).
    static const uint8_t kAlgList[] = {0x80, 0x01, PIV_ALG_ECC_P256, 0x06, 0x01, 0x00};
    if (!tlvWrite(inner, sizeof(inner), &ip, PIV_TAG_ALG_ID, kAlgList, sizeof(kAlgList)))
        return 0;

    size_t op = 0;
    if (!tlvWrite(out, outCap, &op, PIV_TAG_APT, inner, ip)) return 0;
    return op;
}

size_t buildChuid(const uint8_t guid[16], uint8_t* out, size_t outCap) {
    // FASC-N (25 bytes): a fixed non-federal test pattern. Windows/OpenSC do
    // not validate its agency fields; the Card UUID (0x34) is what binds the
    // Windows container id.
    static const uint8_t kFascn[25] = {
        0xD4, 0xE7, 0x39, 0xDA, 0x73, 0x9C, 0xED, 0x39, 0xCE, 0x73, 0x9D, 0x83,
        0x68, 0x58, 0x21, 0x08, 0x42, 0x10, 0x84, 0x21, 0xC8, 0x42, 0x10, 0xC3,
        0xEB
    };
    static const char kExpiry[] = "20351231";  // tag 0x35, ASCII YYYYMMDD

    size_t p = 0;
    if (!tlvWrite(out, outCap, &p, 0x30, kFascn, sizeof(kFascn))) return 0;
    if (!tlvWrite(out, outCap, &p, 0x34, guid, 16)) return 0;
    if (!tlvWrite(out, outCap, &p, 0x35,
                  reinterpret_cast<const uint8_t*>(kExpiry), sizeof(kExpiry) - 1))
        return 0;
    if (!tlvWrite(out, outCap, &p, 0x3E, nullptr, 0)) return 0;  // empty issuer signature
    if (!tlvWrite(out, outCap, &p, 0xFE, nullptr, 0)) return 0;  // LRC (length 0)
    return p;
}

size_t buildCcc(const uint8_t cardId[14], uint8_t* out, size_t outCap) {
    // F0: Card Identifier (21 bytes): fixed GSC-RID prefix + 14 random bytes.
    uint8_t f0[21] = {
        0xA0, 0x00, 0x00, 0x01, 0x16, 0xFF, 0x21  // GSC-IS RID + manufacturer/card-type
    };
    memcpy(f0 + 7, cardId, 14);

    size_t p = 0;
    if (!tlvWrite(out, outCap, &p, 0xF0, f0, sizeof(f0))) return 0;
    // F1/F2: container/CCC version numbers.
    static const uint8_t kV21[1] = {0x21};
    if (!tlvWrite(out, outCap, &p, 0xF1, kV21, 1)) return 0;
    if (!tlvWrite(out, outCap, &p, 0xF2, kV21, 1)) return 0;
    // F3 (app card URL), F4 (PKCS#15), F5 (registered data model = 0x10),
    // F6/F7 (containers), FA-FE: all present, mostly empty per the standard
    // static CCC layout that ykman generates.
    if (!tlvWrite(out, outCap, &p, 0xF3, nullptr, 0)) return 0;
    static const uint8_t kZero[1] = {0x00};
    if (!tlvWrite(out, outCap, &p, 0xF4, kZero, 1)) return 0;
    static const uint8_t kDataModel[1] = {0x10};
    if (!tlvWrite(out, outCap, &p, 0xF5, kDataModel, 1)) return 0;
    if (!tlvWrite(out, outCap, &p, 0xF6, nullptr, 0)) return 0;
    if (!tlvWrite(out, outCap, &p, 0xF7, nullptr, 0)) return 0;
    if (!tlvWrite(out, outCap, &p, 0xFA, nullptr, 0)) return 0;
    if (!tlvWrite(out, outCap, &p, 0xFB, nullptr, 0)) return 0;
    if (!tlvWrite(out, outCap, &p, 0xFC, nullptr, 0)) return 0;
    if (!tlvWrite(out, outCap, &p, 0xFD, nullptr, 0)) return 0;
    if (!tlvWrite(out, outCap, &p, 0xFE, nullptr, 0)) return 0;  // LRC
    return p;
}

size_t buildDiscovery(uint8_t* out, size_t outCap) {
    // 7E { 4F <full AID>, 5F2F 02 <PIN usage policy> }
    uint8_t inner[32];
    size_t ip = 0;
    if (!tlvWrite(inner, sizeof(inner), &ip, PIV_TAG_AID, kPivAidFull, sizeof(kPivAidFull)))
        return 0;
    // 5F2F: PIN usage policy. 0x40 0x00 = application PIN only, contact.
    static const uint8_t kPinPolicy[2] = {0x40, 0x00};
    if (!tlvWrite(inner, sizeof(inner), &ip, 0x5F2F, kPinPolicy, sizeof(kPinPolicy)))
        return 0;

    size_t op = 0;
    if (!tlvWrite(out, outCap, &op, PIV_TAG_DISCOVERY, inner, ip)) return 0;
    return op;
}

} // namespace cdc::mod_piv
