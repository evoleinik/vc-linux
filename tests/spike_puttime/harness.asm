;  Test harness: VC 4.99.09 PutTime + BinDec, copied verbatim from VCSUBS.INC.
		.MODEL	Tiny,C
		.CODE
		.STARTUP
IFDEF	__JWASM__
		OPTION oldstructs
ENDIF
		JMP	SHORT Go
Tbl		DW	OFFSET Country, OFFSET Ent, OFFSET OutBuf, OFFSET Done
Go:		MOV	SI,OFFSET Ent
		MOV	DI,OFFSET OutBuf
		CALL	PutTime
Done:		JMP	Done

CNTRY		STRUC
  DateFmt	DW	?		; Date format
		DB	5 DUP(?)	; Currency
  Sep1000	DB	2 DUP(?)	; Thousand separator
  DecSep	DB	2 DUP(?)	; Decimal separator
  DateSep	DB	2 DUP(?)	; Date separator
  TimeSep	DB	2 DUP(?)	; Time separator
		DB	?		; Currency format
		DB	?		; Number of meaning digits in currency
  TimeFmt	DB	?		; Time format
  CaseMap	DD	?		; Address of case map
		DB	2 DUP(?)	; Data separator
		DB	8 DUP(?)	; Reserved
CNTRY		ENDS
FILENT		STRUC
  AttrF		DB	?
  TimeF		DW	?
  DateF		DW	?
  SizeF		DD	?
  CompF		DD	?
  NumbF		DW	?
  DescrF	DW	?
  LenShortNamF	DW	?
  LenLongNamF	DW	?
  NameF		EQU	BYTE PTR $
FILENT		ENDS

PutTime		PROC	FAR	USES	AX BX CX DX
		CMP	ES:Country.TimeFmt,0
		JZ	@@Space
		MOV	AL,' '
		STOSB
  @@Space:	MOV	AX,DS:[SI].TimeF
		PUSH	AX
		AND	AX,WORD PTR 1Fh
		ADD	AX,AX
		CALL	BinDec
		MOV	BX,AX
		POP	AX
		MOV	CL,5
		SHR	AX,CL
		PUSH	AX
		AND	AX,WORD PTR 3Fh
		CALL	BinDec
		MOV	DX,AX
		POP	AX
		MOV	CL,6
		SHR	AX,CL
		AND	AX,WORD PTR 1Fh
		CMP	ES:Country.TimeFmt,0
		JNE	@@ConvHours
		MOV	CL,12
		DIV	CL
		OR	AH,AH
		JNE	@@ChkAP
		MOV	AH,12
  @@ChkAP:	CMP	AL,0
		MOV	AL,AH
		MOV	CH,'a'
		JE	@@ConvHours
		MOV	CH,'p'
  @@ConvHours:	CALL	BinDec
		CMP	AL,'0'
		JNE	@@Write
		MOV	AL,' '
  @@Write:	STOSW
		MOV	AL,ES:Country.TimeSep
		PUSH	AX
		STOSB
		MOV	AX,DX
		STOSW
		POP	AX
		STOSB
		MOV	AX,BX
		STOSW
		CMP	ES:Country.TimeFmt,0
		JNE	@@Exit
		MOV	AL,CH
		STOSB
  @@Exit:	MOV	BYTE PTR ES:[DI],0
		RET
PutTime		ENDP
BinDec		PROC	FAR	USES	CX
		MOV	AH,0
		MOV	CL,10
		DIV	CL
		ADD	AX,'00'
		RET
BinDec		ENDP

		.DATA
Country		CNTRY	<>
Ent		FILENT	<>
OutBuf		DB	32 DUP(0EEh)
		END
