"use strict";

const crypto = require("crypto");
const fs = require("fs");

if ((process.argv.length !== 7) && (process.argv.length !== 8)) {
    console.error(
        "Usage: node sign_mcuboot_keyhash.js <unsigned.bin> <private.pem> " +
        "<template-signed.bin> <version> <output.bin> [confirmed|pending]"
    );
    process.exit(2);
}

const [
    ,
    ,
    unsignedPath,
    privateKeyPath,
    templatePath,
    versionText,
    outputPath,
    trailerMode = "pending",
] = process.argv;

const versionMatch = /^(\d+)\.(\d+)\.(\d+)\+(\d+)$/.exec(versionText);
if (!versionMatch) {
    throw new Error(`Invalid MCUboot version: ${versionText}`);
}
if ((trailerMode !== "confirmed") && (trailerMode !== "pending")) {
    throw new Error(`Invalid trailer mode: ${trailerMode}`);
}

const rawImage = fs.readFileSync(unsignedPath);
const privateKey = crypto.createPrivateKey(fs.readFileSync(privateKeyPath));
if (privateKey.asymmetricKeyType !== "ec") {
    throw new Error("Signing key must be an EC private key");
}
if (privateKey.asymmetricKeyDetails &&
    privateKey.asymmetricKeyDetails.namedCurve &&
    !["prime256v1", "P-256"].includes(privateKey.asymmetricKeyDetails.namedCurve)) {
    throw new Error(`Signing key must use P-256, got ${privateKey.asymmetricKeyDetails.namedCurve}`);
}

const publicKey = crypto.createPublicKey(privateKey);
const publicKeyDer = publicKey.export({ type: "spki", format: "der" });
const template = fs.readFileSync(templatePath);
const headerSize = 0x200;
const slotSize = 0x60000;

if (rawImage.length === 0) {
    throw new Error("Application image is empty");
}
if (rawImage.length + headerSize >= slotSize - 32) {
    throw new Error("Application does not fit in the 0x60000-byte MCUboot slot");
}
if (template.length !== slotSize) {
    throw new Error(`Template slot must be ${slotSize} bytes`);
}

const header = Buffer.alloc(headerSize, 0xff);
header.writeUInt32LE(0x96f3b83d, 0);
header.writeUInt32LE(0, 4);
header.writeUInt16LE(headerSize, 8);
header.writeUInt16LE(0, 10);
header.writeUInt32LE(rawImage.length, 12);
header.writeUInt32LE(0, 16);
header.writeUInt8(Number(versionMatch[1]), 20);
header.writeUInt8(Number(versionMatch[2]), 21);
header.writeUInt16LE(Number(versionMatch[3]), 22);
header.writeUInt32LE(Number(versionMatch[4]), 24);
header.writeUInt32LE(0, 28);

const signedPayload = Buffer.concat([header, rawImage]);
const imageHash = crypto.createHash("sha256").update(signedPayload).digest();
const keyHash = crypto.createHash("sha256").update(publicKeyDer).digest();
const signature = crypto.sign("sha256", signedPayload, privateKey);
if (!crypto.verify("sha256", signedPayload, publicKey, signature)) {
    throw new Error("ECDSA self-verification failed");
}

function encodeTlv(type, value) {
    const entry = Buffer.alloc(4 + value.length);
    entry.writeUInt8(type, 0);
    entry.writeUInt8(0, 1);
    entry.writeUInt16LE(value.length, 2);
    value.copy(entry, 4);
    return entry;
}

const entries = [
    encodeTlv(0x10, imageHash),
    encodeTlv(0x01, keyHash),
    encodeTlv(0x22, signature),
];
const tlvTotal = 4 + entries.reduce((total, entry) => total + entry.length, 0);
const tlvHeader = Buffer.alloc(4);
tlvHeader.writeUInt16LE(0x6907, 0);
tlvHeader.writeUInt16LE(tlvTotal, 2);

const output = Buffer.alloc(slotSize, 0xff);
let offset = 0;
for (const part of [signedPayload, tlvHeader, ...entries]) {
    part.copy(output, offset);
    offset += part.length;
}

if (trailerMode === "confirmed") {
    template.subarray(slotSize - 32).copy(output, slotSize - 32);
} else {
    template.subarray(slotSize - 16).copy(output, slotSize - 16);
}

fs.writeFileSync(outputPath, output);
console.log(`OUTPUT=${outputPath}`);
console.log(`VERSION=${versionText}`);
console.log(`IMAGE_SIZE=${rawImage.length}`);
console.log(`SIGNED_END=0x${offset.toString(16).toUpperCase()}`);
console.log(`TRAILER_MODE=${trailerMode}`);
console.log(`KEYHASH=${keyHash.toString("hex").toUpperCase()}`);
console.log("ECDSA_SELF_VERIFY=OK");
