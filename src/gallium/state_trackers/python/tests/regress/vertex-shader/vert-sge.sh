VERT1.1

DCL IN[0], POSITION
DCL IN[1], COLOR
DCL OUT[0], POSITION
DCL OUT[1], COLOR

DCL TEMP[0]

IMM FLT32 { -0.1, -0.1, 1.0, 0.0 }

SGE TEMP[0], IN[0], IMM[0]
MOV OUT[0], IN[0]
MUL OUT[1], IN[1], TEMP[0]

END