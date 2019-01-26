import sys
import binascii
import os
import pathlib


def printProgressBar(progress):
    i = int(progress * 20)
    sys.stdout.write('\r')
    sys.stdout.write("[%-20s] %d%%" % ('='*i, 5*i))
    sys.stdout.flush()

def openFileToByte_generator(filename , chunkSize = 128):
    fileSize = os.stat(filename).st_size
    readBytes = 0.0
    with open(filename, "rb") as f:
        while True:
            chunk = f.read(chunkSize)
            readBytes += chunkSize
            printProgressBar(readBytes/float(fileSize))
            if chunk:
                for byte in chunk:
                    yield byte
            else:
                break

payload_file = os.path.join(os.path.dirname(os.path.realpath(__file__)), "../payload/output/payload.bin")

if not os.path.isfile(payload_file):
	print("Error, payload file not found!!")
	exit(1)

fileOut = str(pathlib.Path(payload_file).parents[4].joinpath("nro/source/payload.h"))

if os.path.isfile(fileOut):
	print("payload header file exists, skipping ...")
	exit(0)

stringBuffer = "\t"
countBytes = 0


for byte in openFileToByte_generator(payload_file,16):
    countBytes += 1
    stringBuffer += "0x"+binascii.hexlify(bytes(byte)).decode('ascii')+", "
    if countBytes%16 is 0:
    	stringBuffer += "\n\t"

stringBuffer = "#define PAYLOAD_BIN_SIZE " + str(countBytes) + "\nconst  u8 payloadBin[PAYLOAD_BIN_SIZE] = {\n" + stringBuffer + "\n};"

print("\nwriting file: " + fileOut)
text_file = open(fileOut, "w")
text_file.write(stringBuffer)
text_file.close()

print("finished")

