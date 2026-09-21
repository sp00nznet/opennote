#ifndef FILL_REPO_H
#define FILL_REPO_H

// What somebody brings to a form: their signature, and their answers.
//
// A form asks the same questions every other form asks -- a name, an address,
// a date -- and then asks you to sign it. Doing that once and keeping the
// result is the whole idea: the signature is drawn once and reused, and an
// answer typed into one form is offered to the next one that asks for the
// same thing.
//
// It lives in the same SQLite file as the notes, which is the only file this
// program writes. Nothing leaves the machine.

#define MAX_SIGNATURES  32
#define MAX_ANSWERS     256

typedef struct {
    int   id;
    WCHAR name[64];
    WCHAR createdAt[32];
    int   bytes;
} SignatureInfo;

// Keep a signature. The bytes are a PNG, transparent where the paper should
// show through. Returns its id, or 0.
int  Fill_SaveSignature(const WCHAR* name, const BYTE* png, size_t len);

// The ones already kept, newest first.
int  Fill_ListSignatures(SignatureInfo* out, int max);

// The picture itself. Caller frees.
BYTE* Fill_ReadSignature(int id, size_t* lenOut);
BOOL  Fill_DeleteSignature(int id);

// ---------------------------------------------------------------------------
// Answers
// ---------------------------------------------------------------------------

// Remember what was typed into a field of this name. Field names are matched
// loosely -- case and punctuation are ignored -- because "Full Name",
// "full_name" and "FullName" are the same question asked by three different
// form designers.
BOOL Fill_RememberAnswer(const WCHAR* field, const WCHAR* value);

// What was typed into a field of that name before, if anything. Returns FALSE
// when nothing is remembered, which is not an error.
BOOL Fill_RecallAnswer(const WCHAR* field, WCHAR* out, size_t outChars);

// Every remembered answer, for showing and forgetting.
typedef struct {
    WCHAR field[128];
    WCHAR value[512];
    WCHAR updatedAt[32];
} RememberedAnswer;

int  Fill_ListAnswers(RememberedAnswer* out, int max);
BOOL Fill_ForgetAnswer(const WCHAR* field);
BOOL Fill_ForgetAllAnswers(void);

// Self-check, run by `OpenNote.exe --selftest`. Uses a database of its own in
// the temp directory and deletes it afterwards.
BOOL Fill_SelfTest(char* failure, size_t failureSize);

#endif // FILL_REPO_H
