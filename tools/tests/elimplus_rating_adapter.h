// Stand-ins for the engine types ElimPlusRatingSystem.cpp uses, so that the WHOLE production translation unit and
// the vendored TeamGlicko2 library compile and run outside UBT (test_elimplus_rating_flow.py builds them with
// elimplus_rating_cases.cpp). Nothing here re-implements rating logic: these are containers, strings and logging.
//
// Mods.db is not in this process. Every ExecDatabaseCommand is written to stdout and answered on stdin by the
// Python test, which runs it on real sqlite and hands rows back in the engine wrapper's format. So the test owns
// the database, can share it between several of these processes, and can make any single statement fail.
//
// Where the engine's behaviour matters to the code under test it is copied: FString compares without case,
// FMath::RoundToInt takes a float, TMap iterates in insertion order, TArray::Contains compares elements.
//
// Known differences from the engine, which these tests therefore cannot see:
//  - TCHAR is narrow here and wide in the engine, so a format-specifier fault that only shows with wide strings
//    passes. UBT and a real server are the check for those.
//  - A failed statement comes back with no rows. The engine's ExecDatabaseCommand can return false after it has
//    already appended rows.
//  - This TMap keeps its elements in a deque, so a reference stays valid across a later Add. The engine's TMap
//    may move its elements on Add, so code that held a reference across one would only break in the engine.
//  - FDateTime prints a simplified ISO timestamp, and rounding uses this platform's C library.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

typedef int32_t int32;
typedef long long int64;
typedef uint8_t uint8;
typedef uint32_t uint32;
typedef char TCHAR;
#define TEXT(x) x
#define NETCODEPLUS_API

const char kStandInSeparator = '\x1f';

template <class T> typename std::remove_reference<T>::type&& MoveTemp(T&& Value)
{
	return static_cast<typename std::remove_reference<T>::type&&>(Value);
}

// ---------------------------------------------------------------- strings
inline int StandInStricmp(const std::string& A, const std::string& B)
{
	const size_t Shared = A.size() < B.size() ? A.size() : B.size();
	for (size_t i = 0; i < Shared; ++i)
	{
		const int Left = std::tolower(static_cast<unsigned char>(A[i]));
		const int Right = std::tolower(static_cast<unsigned char>(B[i]));
		if (Left != Right)
		{
			return Left < Right ? -1 : 1;
		}
	}
	return A.size() == B.size() ? 0 : (A.size() < B.size() ? -1 : 1);
}

struct FString : std::string
{
	FString() {}
	FString(const char* In) : std::string(In ? In : "") {}
	FString(const std::string& In) : std::string(In) {}

	static FString Printf(const char* Format, ...)
	{
		va_list Args;
		va_start(Args, Format);
		va_list Measure;
		va_copy(Measure, Args);
		const int Needed = std::vsnprintf(nullptr, 0, Format, Measure);
		va_end(Measure);
		std::string Out(Needed > 0 ? static_cast<size_t>(Needed) : 0, '\0');
		if (Needed > 0)
		{
			std::vsnprintf(&Out[0], static_cast<size_t>(Needed) + 1, Format, Args);
		}
		va_end(Args);
		return FString(Out);
	}

	const char* operator*() const { return c_str(); }
	bool IsEmpty() const { return empty(); }
	int32 Len() const { return static_cast<int32>(size()); }

	FString Replace(const char* From, const char* To) const
	{
		std::string Out(*this);
		const size_t FromLength = std::strlen(From);
		const size_t ToLength = std::strlen(To);
		for (size_t At = Out.find(From); FromLength && At != std::string::npos; At = Out.find(From, At + ToLength))
		{
			Out.replace(At, FromLength, To);
		}
		return FString(Out);
	}

	bool StartsWith(const FString& Prefix) const
	{
		return size() >= Prefix.size() && StandInStricmp(substr(0, Prefix.size()), Prefix) == 0;
	}

	bool RemoveFromStart(const FString& Prefix)
	{
		if (Prefix.empty() || !StartsWith(Prefix))
		{
			return false;
		}
		erase(0, Prefix.size());
		return true;
	}
};

inline bool operator==(const FString& A, const FString& B) { return StandInStricmp(A, B) == 0; }
inline bool operator==(const FString& A, const char* B) { return StandInStricmp(A, B ? B : "") == 0; }
inline bool operator!=(const FString& A, const FString& B) { return StandInStricmp(A, B) != 0; }
inline bool operator!=(const FString& A, const char* B) { return StandInStricmp(A, B ? B : "") != 0; }

struct FCString
{
	static int32 Atoi(const char* In) { return static_cast<int32>(std::strtol(In, nullptr, 10)); }
	static double Atod(const char* In) { return std::strtod(In, nullptr); }
};

// ---------------------------------------------------------------- containers
template <class T> struct TArray : std::vector<T>
{
	int32 Num() const { return static_cast<int32>(this->size()); }
	int32 Add(const T& Value) { this->push_back(Value); return Num() - 1; }
	int32 Add(T&& Value) { this->push_back(std::move(Value)); return Num() - 1; }
	void Reserve(size_t Count) { this->reserve(Count); }
	void Empty() { this->clear(); }
	bool Contains(const T& Value) const
	{
		for (const T& Element : *this)
		{
			if (Element == Value)
			{
				return true;
			}
		}
		return false;
	}
};

template <class K, class V> struct TPair
{
	K Key;
	V Value;
	TPair(const K& InKey, const V& InValue) : Key(InKey), Value(InValue) {}
};

/** Insertion-ordered, like the engine's TMap while nothing has been removed. A deque keeps references to values
 *  valid across later additions, as the production code assumes of FindOrAdd / Add. */
template <class K, class V> struct TMap
{
	typedef std::deque<TPair<K, V>> FStorage;
	FStorage Pairs;

	typename FStorage::iterator begin() { return Pairs.begin(); }
	typename FStorage::iterator end() { return Pairs.end(); }
	typename FStorage::const_iterator begin() const { return Pairs.begin(); }
	typename FStorage::const_iterator end() const { return Pairs.end(); }

	V* Find(const K& Key)
	{
		for (TPair<K, V>& Pair : Pairs)
		{
			if (Pair.Key == Key)
			{
				return &Pair.Value;
			}
		}
		return nullptr;
	}
	const V* Find(const K& Key) const { return const_cast<TMap*>(this)->Find(Key); }
	bool Contains(const K& Key) const { return Find(Key) != nullptr; }
	V FindRef(const K& Key) const
	{
		const V* Existing = Find(Key);
		return Existing ? *Existing : V();
	}
	V& Add(const K& Key, const V& Value)
	{
		if (V* Existing = Find(Key))
		{
			*Existing = Value;
			return *Existing;
		}
		Pairs.emplace_back(Key, Value);
		return Pairs.back().Value;
	}
	V& FindOrAdd(const K& Key)
	{
		if (V* Existing = Find(Key))
		{
			return *Existing;
		}
		Pairs.emplace_back(Key, V());
		return Pairs.back().Value;
	}
	V& operator[](const K& Key)
	{
		V* Existing = Find(Key);
		if (!Existing)
		{
			std::fprintf(stderr, "TMap::operator[] on a missing key\n");
			std::abort();
		}
		return *Existing;
	}
	const V& operator[](const K& Key) const { return const_cast<TMap*>(this)->operator[](Key); }
	int32 Remove(const K& Key)
	{
		int32 Removed = 0;
		for (typename FStorage::iterator It = Pairs.begin(); It != Pairs.end();)
		{
			if (It->Key == Key)
			{
				It = Pairs.erase(It);
				++Removed;
			}
			else
			{
				++It;
			}
		}
		return Removed;
	}
	int32 Num() const { return static_cast<int32>(Pairs.size()); }
	void Empty() { Pairs.clear(); }
	void Reserve(size_t) {}
};

template <class T> struct TSet
{
	std::vector<T> Items;

	typename std::vector<T>::const_iterator begin() const { return Items.begin(); }
	typename std::vector<T>::const_iterator end() const { return Items.end(); }
	bool Contains(const T& Value) const
	{
		for (const T& Item : Items)
		{
			if (Item == Value)
			{
				return true;
			}
		}
		return false;
	}
	void Add(const T& Value)
	{
		if (!Contains(Value))
		{
			Items.push_back(Value);
		}
	}
	int32 Remove(const T& Value)
	{
		for (size_t i = 0; i < Items.size(); ++i)
		{
			if (Items[i] == Value)
			{
				Items.erase(Items.begin() + static_cast<std::ptrdiff_t>(i));
				return 1;
			}
		}
		return 0;
	}
	int32 Num() const { return static_cast<int32>(Items.size()); }
	void Empty() { Items.clear(); }
	void Reserve(size_t Count) { Items.reserve(Count); }
};

template <class T> using TUniquePtr = std::unique_ptr<T>;
template <class T, class... TArgs> TUniquePtr<T> MakeUnique(TArgs&&... Args)
{
	return TUniquePtr<T>(new T(std::forward<TArgs>(Args)...));
}

template <class T> struct TSharedRef
{
	std::shared_ptr<T> Object;
	T* operator->() const { return Object.get(); }
};

// ---------------------------------------------------------------- maths, time
struct FMath
{
	/** The engine's takes a float: a double argument is narrowed first, exactly as in the shipped build. */
	static int32 RoundToInt(float Value) { return static_cast<int32>(std::floor(Value + 0.5f)); }
	template <class T> static T Min(const T A, const T B) { return A < B ? A : B; }
	template <class T> static T Max(const T A, const T B) { return A < B ? B : A; }
	template <class T> static T Clamp(const T X, const T Low, const T High) { return X < Low ? Low : (X < High ? X : High); }
	static bool IsNearlyEqual(double A, double B, double Tolerance) { return std::fabs(A - B) <= Tolerance; }
	static int32 RandRange(int32 Low, int32 High) { return Low + std::rand() % (High - Low + 1); }
};

/** The clock the production code reads, set by the test ("now" command) so that 30-day windows are repeatable. */
inline long long& StandInUnixNow()
{
	static long long Now = 1795000000;
	return Now;
}

struct FDateTime
{
	long long Unix;
	static FDateTime UtcNow() { return FDateTime{StandInUnixNow()}; }
	int64 ToUnixTimestamp() const { return Unix; }
	FString ToIso8601() const { return FString::Printf("unix-%lld", Unix); }
};

// ---------------------------------------------------------------- logging, console variables
inline void StandInEmit(const char* Kind, const std::vector<std::string>& Fields)
{
	std::fputs(Kind, stdout);
	for (const std::string& Field : Fields)
	{
		std::fputc(kStandInSeparator, stdout);
		std::fputs(Field.c_str(), stdout);
	}
	std::fputc('\n', stdout);
	std::fflush(stdout);
}

#define DEFINE_LOG_CATEGORY_STATIC(Name, DefaultVerbosity, CompileTimeVerbosity) static const char* const Name = #Name
#define UE_LOG(Category, Verbosity, ...) StandInEmit("LOG", {Category, #Verbosity, FString::Printf(__VA_ARGS__)})

enum { ECVF_Default = 0 };

inline std::map<std::string, void*>& StandInConsoleVariables()
{
	static std::map<std::string, void*> Registry;
	return Registry;
}

template <class T> struct TAutoConsoleVariable
{
	T Value;
	TAutoConsoleVariable(const char* Name, const T& Default, const char*, int = ECVF_Default) : Value(Default)
	{
		StandInConsoleVariables()[Name] = this;
	}
	T GetValueOnGameThread() const { return Value; }
};

// ---------------------------------------------------------------- world, database, replicator, uploader
struct FDatabaseRow
{
	TArray<FString> Text;
};

class UGameInstance
{
public:
	virtual ~UGameInstance() {}
};

class UUTGameInstance : public UGameInstance
{
public:
	/** Sends the statement to the test and reads back what the engine wrapper would have produced: rows are
	 *  APPENDED, and the return value is false for a statement that failed. */
	bool ExecDatabaseCommand(const FString& DatabaseCommand, TArray<FDatabaseRow>& DatabaseRows)
	{
		StandInEmit("SQL", {DatabaseCommand});
		std::string Line;
		while (std::getline(std::cin, Line))
		{
			if (Line == "OK")
			{
				return true;
			}
			if (Line == "FAIL")
			{
				return false;
			}
			FDatabaseRow Row;
			size_t Start = Line.find(kStandInSeparator);          // skip the leading "ROW"
			while (Start != std::string::npos)
			{
				const size_t End = Line.find(kStandInSeparator, Start + 1);
				Row.Text.Add(FString(Line.substr(Start + 1, End == std::string::npos ? std::string::npos : End - Start - 1)));
				Start = End;
			}
			DatabaseRows.Add(Row);
		}
		return false;
	}
};

class UWorld
{
public:
	UGameInstance* GameInstance = nullptr;
	FString MapName;
	FString StreamingLevelsPrefix;
	UGameInstance* GetGameInstance() const { return GameInstance; }
	FString GetMapName() const { return MapName; }
};

template <class To, class From> To* Cast(From* Value)
{
	return dynamic_cast<To*>(Value);
}

class AElimPlusStatsReplicator
{
public:
	void SetPlayerEloAndDelta(const FString& UniqueId, int32 Elo, int32 Delta)
	{
		StandInEmit("ELO", {UniqueId, std::to_string(Elo), std::to_string(Delta)});
	}
	void SetPlayerGlobalRank(const FString& UniqueId, int32 Rank)
	{
		StandInEmit("RANK", {UniqueId, std::to_string(Rank)});
	}
};

struct FNCEloUploader
{
	static FString ResolveServerName(UWorld*) { return FString("Stand-in hub"); }
};

// ---------------------------------------------------------------- JSON writer (the calls BuildResultPayload makes)
template <class CharType> struct TCondensedJsonPrintPolicy {};

template <class CharType, class PrintPolicy> class TJsonWriter
{
public:
	explicit TJsonWriter(FString* InOut) : Out(InOut) { First.push_back(true); }

	void WriteObjectStart() { Separate(); *Out += "{"; First.push_back(true); }
	void WriteObjectEnd() { First.pop_back(); *Out += "}"; }
	void WriteArrayStart(const FString& Identifier) { Name(Identifier); *Out += "["; First.push_back(true); }
	void WriteArrayEnd() { First.pop_back(); *Out += "]"; }
	void WriteValue(const FString& Identifier, bool Value) { Name(Identifier); *Out += Value ? "true" : "false"; }
	void WriteValue(const FString& Identifier, int32 Value) { Name(Identifier); *Out += std::to_string(Value); }
	void WriteValue(const FString& Identifier, long long Value) { Name(Identifier); *Out += std::to_string(Value); }
	void WriteValue(const FString& Identifier, double Value) { Name(Identifier); *Out += FString::Printf("%.17g", Value); }
	void WriteValue(const FString& Identifier, const FString& Value) { Name(Identifier); *Out += Quote(Value); }
	void WriteValue(const FString& Identifier, const char* Value) { Name(Identifier); *Out += Quote(Value ? Value : ""); }
	void Close() {}

private:
	void Separate()
	{
		if (!First.back())
		{
			*Out += ",";
		}
		First.back() = false;
	}
	void Name(const FString& Identifier)
	{
		Separate();
		*Out += Quote(Identifier);
		*Out += ":";
	}
	static std::string Quote(const std::string& In)
	{
		std::string Quoted = "\"";
		for (const char Character : In)
		{
			if (Character == '"' || Character == '\\')
			{
				Quoted += '\\';
				Quoted += Character;
			}
			else if (static_cast<unsigned char>(Character) < 0x20)
			{
				char Escape[8];
				std::snprintf(Escape, sizeof(Escape), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(Character)));
				Quoted += Escape;
			}
			else
			{
				Quoted += Character;
			}
		}
		return Quoted + "\"";
	}

	FString* Out;
	std::vector<bool> First;
};

template <class CharType, class PrintPolicy> struct TJsonWriterFactory
{
	static TSharedRef<TJsonWriter<CharType, PrintPolicy>> Create(FString* Out)
	{
		return TSharedRef<TJsonWriter<CharType, PrintPolicy>>{std::make_shared<TJsonWriter<CharType, PrintPolicy>>(Out)};
	}
};
