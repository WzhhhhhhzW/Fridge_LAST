"use strict";

const fs = require("fs");

if (process.argv.length !== 5) {
    console.error("Usage: node mcuboot_sparse_ihex.js <signed-slot.bin> <base-address> <output.hex>");
    process.exit(2);
}

const [, , inputPath, baseText, outputPath] = process.argv;
const image = fs.readFileSync(inputPath);
const baseAddress = Number(baseText);
if (!Number.isInteger(baseAddress) || baseAddress < 0) {
    throw new Error(`Invalid base address: ${baseText}`);
}
if (image.length !== 0x60000 || image.readUInt32LE(0) !== 0x96f3b83d) {
    throw new Error("Input must be a complete signed 0x60000-byte MCUboot slot");
}

const headerSize = image.readUInt16LE(8);
const protectedTlvSize = image.readUInt16LE(10);
const imageSize = image.readUInt32LE(12);
const regularTlvOffset = headerSize + imageSize + protectedTlvSize;
if (image.readUInt16LE(regularTlvOffset) !== 0x6907) {
    throw new Error("Regular MCUboot TLV header not found");
}
const signedEnd = regularTlvOffset + image.readUInt16LE(regularTlvOffset + 2);
const trailerStart = image.length - 32;

function record(address, type, data) {
    const body = Buffer.alloc(4 + data.length);
    body.writeUInt8(data.length, 0);
    body.writeUInt16BE(address & 0xffff, 1);
    body.writeUInt8(type, 3);
    data.copy(body, 4);
    let sum = 0;
    for (const byte of body) {
        sum = (sum + byte) & 0xff;
    }
    const checksum = ((~sum + 1) & 0xff).toString(16).padStart(2, "0");
    return `:${body.toString("hex")}${checksum}`.toUpperCase();
}

const lines = [];
let activeUpper = -1;
function emitChunk(fileOffset, length) {
    for (let offset = 0; offset < length; offset += 16) {
        const absolute = baseAddress + fileOffset + offset;
        const upper = (absolute >>> 16) & 0xffff;
        if (upper !== activeUpper) {
            const upperBytes = Buffer.alloc(2);
            upperBytes.writeUInt16BE(upper, 0);
            lines.push(record(0, 4, upperBytes));
            activeUpper = upper;
        }
        const data = image.subarray(
            fileOffset + offset,
            Math.min(fileOffset + offset + 16, fileOffset + length)
        );
        lines.push(record(absolute, 0, data));
    }
}

emitChunk(0, signedEnd);
emitChunk(trailerStart, 32);
lines.push(":00000001FF");
fs.writeFileSync(outputPath, `${lines.join("\r\n")}\r\n`);

console.log(`OUTPUT=${outputPath}`);
console.log(`BASE=0x${baseAddress.toString(16).toUpperCase()}`);
console.log(`SIGNED_BYTES=${signedEnd}`);
console.log("TRAILER_BYTES=32");
