#include "uberdemotools.h"

#include <vector>
#include <string>
#include <codecvt>
#include <iostream>

#include <fcntl.h>
#include <io.h>

#define BOOST_FILESYSTEM_NO_DEPRECATED
#include "boost\filesystem.hpp"

#include "boost\algorithm\string\trim.hpp"

using std::cout;
using std::endl;

/*
ABOUT THIS APPLICATION
==============================
This is a little application that finds all Quake Live ffa demos in the given folder and sorts them by winner. Then result is copied into a given destination folder
*/

static void PrintHelp()
{
	std::wcout << "Finds all Quake Live ffa demos in the given folder and sorts them by winner." << std::endl;
	std::wcout << "Usage: qlffawinselector demosfolder [targetfolder]" << std::endl;
	std::wcout << "demosfolder - folder with Quake Live ffa demos" << std::endl;
	std::wcout << "targetfolder - optional parameter, folder to which the result will be copied (by default it is 'Output' subfolder)" << std::endl; //TODO: добавить создание дефолтной папки
}

static void CrashCallback(const char* message)
{
	std::wcout << "Fatal error: " << message << std::endl;
	exit(EXIT_FAILURE);
}

static bool ExtractRawPlayerName(std::string& playerName, const std::string& configString)
{
	char name[256];
	char temp[4];
	const s32 errorCode = udtParseConfigStringValueAsString(name, (u32)sizeof(name), temp, (u32)sizeof(temp), "n", configString.c_str());

	if (errorCode != (s32)udtErrorCode::None)
	{
		return false;
	}

	playerName = name;

	return true;
}

static bool ExtractPlayerName(std::wstring& playerName, const std::string& configString, udtProtocol::Id protocol)
{
	std::string rawPlayerName;
	if (!ExtractRawPlayerName(rawPlayerName, configString))
	{
		return false;
	}

	if (rawPlayerName.empty())
	{
		return true;
	}

	std::vector<char> cleanedUpName(rawPlayerName.length() + 1);
	strcpy(&cleanedUpName[0], rawPlayerName.c_str());
	udtCleanUpString(&cleanedUpName[0], (u32)protocol);
	rawPlayerName = &cleanedUpName[0];

	std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
	playerName = conv.from_bytes(rawPlayerName);
	return true;
}

struct FileReader
{
	FileReader()
	{
	}

	~FileReader()
	{
		_file.close();
	}

	bool Open(const boost::filesystem::path& filePath)
	{
		_file.open(filePath, std::ios_base::in | std::ios_base::binary);
		return true;
	}

	bool Read(void* buffer, u32 byteCount)
	{
		_file.read((char*)buffer, byteCount);
		return _file.gcount() == byteCount;
	}

	void Close()
	{
		_file.close();
	}

private:
	FileReader(const FileReader&);
	void operator=(const FileReader&);

	boost::filesystem::ifstream _file;
};

struct WinChecker
{
	WinChecker()
	{
		_cuContext = NULL;
		_inMsgData = NULL;
	}

	~WinChecker()
	{
		if(_cuContext != NULL)
		{
			udtCuDestroyContext(_cuContext);
		}

		free(_inMsgData);
	}

	bool Init()
	{
		udtCuContext* const cuContext = udtCuCreateContext();
		if(cuContext == NULL)
		{
			std::wcout << "Error: udtCuCreateContext failed" << std::endl;
			return false;
		}
		_cuContext = cuContext;
		
		u8* const inMsgData = (u8*)malloc(ID_MAX_MSG_LENGTH);
		if(inMsgData == NULL)
		{
			std::wcout << "Failed to allocated " << (int)ID_MAX_MSG_LENGTH << " bytes" << std::endl;
			return false;
		}
		_inMsgData = inMsgData;

		return true;
	}

	// Reads the demo file and assigns winnerName parameter to won player name. Returns true if operation was successful, false otherwise
	bool ProcessDemoFile(const boost::filesystem::path& filePath, std::wstring& winnerName)
	{
		_leaderName = {};

		std::wcout << "Processing demo file: " << filePath << std::endl;
		bool isgd = std::wcout.good();

		FileReader reader;
		if(!reader.Open(filePath))
		{
			return false;
		}

		// Determine the demo protocol by file extension
		udtProtocol::Id protocol = udtProtocol::Id::Invalid;
		{
			const std::string fileExtension = filePath.extension().string();
			for (u32 iProtocol = udtProtocol::Id::FirstProtocol; iProtocol < udtProtocol::Id::Count; iProtocol++)
			{
				const char* protocolExtension = udtGetFileExtensionByProtocol(iProtocol);
				if (fileExtension == protocolExtension)
					protocol = (udtProtocol::Id)iProtocol;
			}

			if (protocol == udtProtocol::Id::Invalid)
			{
				std::wcout << "Error: invalid demo file extension" << std::endl;
				return false;
			}
		}

		s32 firstPlayerIdx;
		if(udtGetIdMagicNumber(&firstPlayerIdx, (u32)udtMagicNumberType::ConfigStringIndex, (s32)udtConfigStringIndex::FirstPlayer, (udtProtocol::Id)protocol, (u32)udtMod::None) != 
		   udtErrorCode::None)
		{
			std::wcout << "Error: failed to get the index of the first player config string" << std::endl;
			return false;
		}

		s32 errorCode = udtCuStartParsing(_cuContext, protocol);
		if(errorCode != udtErrorCode::None)
		{
			std::wcout << "Error: udtCuStartParsing failed (" << udtGetErrorCodeString(errorCode) << ")" << std::endl;
			return false;
		}

		udtCuMessageInput input;
		udtCuMessageOutput output;
		u32 continueParsing = 0;

		for(;;)
		{
			if(!reader.Read(&input.MessageSequence, 4))
			{
				std::wcout << "Error: demo is truncated" << std::endl;
				return false;
			}

			if(!reader.Read(&input.BufferByteCount, 4))
			{
				std::wcout << "Error: demo is truncated" << std::endl;
				return false;
			}

			if(input.MessageSequence == -1 && input.BufferByteCount == u32(-1))
			{
				// End of demo file.
				break;
			}

			if(input.BufferByteCount > ID_MAX_MSG_LENGTH)
			{
				std::wcout << "Error: corrupt input (the buffer length exceeds the maximum allowed)" << std::endl;
				return false;
			}

			if(!reader.Read(_inMsgData, input.BufferByteCount))
			{
				std::wcout << "Error: demo is truncated" << std::endl;
				return false;
			}
			input.Buffer = _inMsgData;

			errorCode = udtCuParseMessage(_cuContext, &output, &continueParsing, &input);
			if(errorCode != udtErrorCode::None)
			{
				std::wcout << "Error: udtCuParseMessage failed (" << udtGetErrorCodeString(errorCode) << ")" << std::endl;
				return false;
			}

			if(continueParsing == 0)
				break;

			AnalyzeMessage(output, firstPlayerIdx, protocol);
		}

		winnerName = _leaderName;

		if (winnerName.empty())
		{
			std::wcout << "No 'scores_ffa' messages" << std::endl;
			return false;
		}

		return true;
	}

private:
	WinChecker(const WinChecker&);
	void operator=(const WinChecker&);

	void AnalyzeMessage(const udtCuMessageOutput& message, s32 firstPlayerIdx, udtProtocol::Id protocol)
	{
		if (!message.IsGameState && message.Commands && message.GameStateOrSnapshot.Snapshot && message.CommandCount > 0)
		{
			for(u32 i = 0; i < message.CommandCount; ++i)
				AnalyzeCommand(message.Commands[i], firstPlayerIdx, protocol);
		}
	}

	void AnalyzeCommand(const udtCuCommandMessage& command, s32 firstPlayerIdx, udtProtocol::Id protocol)
	{
		if (command.TokenCount == 0)
			return;

		if (command.CommandTokens[0] != std::string{"scores_ffa"})
			return;

		const int numRecords = std::stoi(command.CommandTokens[1]); // number of player stats records
		if (numRecords <= 0)
			return;

		const int leaderPlayerId = std::stoi(command.CommandTokens[4]);
		udtCuConfigString cs;
		udtCuGetConfigString(_cuContext, &cs, firstPlayerIdx + leaderPlayerId);
		if (cs.ConfigString && cs.ConfigStringLength > 0)
		{
			std::wstring playerName;
			if (ExtractPlayerName(playerName, cs.ConfigString, protocol))
			{
				_leaderName = playerName;
			}
			else
			{
				std::wcout << "Error: cannot extract player name from config" << std::endl;
			}
		}
		else
		{
			std::wcout << "Error: config for player id not found" << std::endl;
		}
	}
	
	std::wstring _leaderName; // leader in the last handled message "scores_ffa"
	udtCuContext* _cuContext;
	u8* _inMsgData;
};

// Correct player name into string what can be used as a folder name. Replace all invalid symbols to "~"
void CorrectPlayerNameToFolderName(std::wstring& winnerName)
{
	if (winnerName.empty())
	{
		winnerName = L"_empty_name_";
		return;
	}
	else if (winnerName == L".")
	{
		winnerName = L"~";
		return;
	}
	else if (winnerName == L"..")
	{
		winnerName = L"~~";
		return;
	}
	else
	{
		boost::algorithm::trim(winnerName);

		if (winnerName.back() == L'.')
			winnerName.back() = L'~';

		for (wchar_t& ch: winnerName)
		{
			if (ch >= L'\x00' && ch <= L'\x1F')
			{
				ch = L'~';
				continue;
			}

			const std::wstring invalid_chars = L"<>:\"/\\|*?";
			if (invalid_chars.find(ch) != std::wstring::npos)
				ch = L'~';
		}
	}
}

void HandleError(const boost::filesystem::filesystem_error& ex)
{
	const std::string error{ex.what()};
	int errorSize = MultiByteToWideChar(boost::winapi::CP_ACP_, boost::winapi::MB_PRECOMPOSED_, error.c_str(), error.size(), nullptr, 0);
	std::wstring werror(errorSize, L'\0');
	MultiByteToWideChar(boost::winapi::CP_ACP_, boost::winapi::MB_PRECOMPOSED_, error.c_str(), error.size(), &werror[0], errorSize);
	std::wcout << "Error: " << werror << std::endl;
}

int wmain(int argc, wchar_t** argv)
{
	_setmode(_fileno(stdout), _O_U8TEXT); // for utf-8 output

	if (!udtSameVersion())
	{
		std::wcout << "Error: compiled with header for version " << UDT_VERSION_STRING << ", but linked against version " << udtGetVersionString() << std::endl;
		return 1;
	}

	if (argc < 2)
	{
		PrintHelp();
		return 1;
	}

	try
	{
		// Check if demos folder exists
		const boost::filesystem::path demosFolder(argv[1]);
		if (!boost::filesystem::is_directory(demosFolder))
		{
			std::wcout << demosFolder << " - no such folder" << std::endl;
			return 1;
		}


		boost::filesystem::path targetFolder(argc >= 3 ? argv[2] : demosFolder / "Output");

		if (!boost::filesystem::is_directory(targetFolder)) //check if target folder not exists
		{
			if (!boost::filesystem::create_directories(targetFolder))
			{
				std::wcout << "Error: cannot create folder " << targetFolder << std::endl;
				return 1;
			}
		}

		udtSetCrashHandler(&CrashCallback);
		udtInitLibrary();

		WinChecker winChecker;
		if (!winChecker.Init())
			return 1;

		for (const boost::filesystem::directory_entry& file: boost::filesystem::directory_iterator(demosFolder))
		{
			try
			{
				if (!boost::filesystem::is_regular_file(file))
					continue;

				std::wstring winnerName;
				if (winChecker.ProcessDemoFile(file, winnerName))
				{
					CorrectPlayerNameToFolderName(winnerName);
				
					const boost::filesystem::path winnerFolder{targetFolder / winnerName};
					if (!boost::filesystem::is_directory(winnerFolder)) //check if winner folder not exists
					{
						if (!boost::filesystem::create_directory(winnerFolder))
						{
							std::wcout << "Error: cannot create folder " << winnerFolder << std::endl;
							continue;
						}
					}
					boost::filesystem::copy_file(file, winnerFolder / file.path().filename(), boost::filesystem::copy_option::overwrite_if_exists);
				}
			}
			catch (const boost::filesystem::filesystem_error& ex)
			{
				HandleError(ex);
			}
		}
	}
	catch (const boost::filesystem::filesystem_error& ex)
	{
		HandleError(ex);
	}

	udtShutDownLibrary();

	return 0;
}