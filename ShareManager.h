/*
 * Copyright (C) 2001-2011 Jacek Sieka, arnetheduck on gmail point com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef DCPLUSPLUS_DCPP_SHARE_MANAGER_H
#define DCPLUSPLUS_DCPP_SHARE_MANAGER_H

#include "TimerManager.h"
#include "SearchManager.h"
#include "SettingsManager.h"
#include "HashManager.h"
#include "QueueManagerListener.h"

#include "Exception.h"
#include "Thread.h"
#include "StringSearch.h"
#include "Singleton.h"
#include "BloomFilter.h"
#include "MerkleTree.h"
#include "Pointer.h"
#include "LogManager.h"
#include "pme.h"
#include "AirUtil.h"

namespace dcpp {

STANDARD_EXCEPTION(ShareException);
static string FileListALL = "All";

class SimpleXML;
class Client;
class File;
class OutputStream;
class MemoryInputStream;
struct ShareLoader;
class Worker;

class ShareManager : public Singleton<ShareManager>, private Thread, private SettingsManagerListener, private TimerManagerListener, private QueueManagerListener
{
public:
	/**
	 * @param aDirectory Physical directory location
	 * @param aName Virtual name
	 */
	void addDirectory(const string& realPath, const string &virtualName);
	void removeDirectory(const string& realPath);
	void renameDirectory(const string& realPath, const string& virtualName);

	string toVirtual(const TTHValue& tth, Client* client) const;
	string toReal(const string& virtualFile, bool isInSharingHub, const HintedUser& aUser, const string& userSID);
	pair<string, int64_t> toRealWithSize(const string& virtualFile, bool isInSharingHub, const HintedUser& aUser, const string& userSID);
	TTHValue getTTH(const string& virtualFile, const HintedUser& aUser, const string& userSID) const;
	
	int refresh(int refreshOptions);
	int initRefreshThread(int refreshOptions) noexcept;
	int refresh(const string& aDir);
	
	//need to be called from inside a lock.
	void setDirty(bool force = false) {
		for(auto i = fileLists.begin(); i != fileLists.end(); ++i) {
			i->second->xmlDirty = true;
			if(force)
				i->second->forceXmlRefresh = true;
		}
		ShareCacheDirty = true; 
	}
	
	void setHubFileListDirty(const string& HubUrl) {
		RLock l(cs);
		auto i = fileLists.find(AirUtil::stripHubUrl(HubUrl));
		if(i != fileLists.end())
			i->second->forceXmlRefresh = true;
	}

	StringList getIncoming() { return incoming; };
	void setIncoming(const string& realPath) { incoming.push_back(realPath); };
	void DelIncoming() { incoming.clear(); };

   void save() { 
		w.join();
		//LogManager::getInstance()->message("Creating share cache...");
		w.start();
	}

	void Startup() {
		AirUtil::updateCachedSettings();
		if(!loadCache())
			refresh(REFRESH_ALL | REFRESH_BLOCKING);
	}

	void shutdown();
	bool shareFolder(const string& path, bool thoroughCheck = false, bool QuickCheck = false) const;
	int64_t removeExcludeFolder(const string &path, bool returnSize = true);
	int64_t addExcludeFolder(const string &path);

	void search(SearchResultList& l, const string& aString, int aSearchType, int64_t aSize, int aFileType, Client* aClient, StringList::size_type maxResults) noexcept;
	void search(SearchResultList& l, const StringList& params, StringList::size_type maxResults, const Client* client, const CID& cid) noexcept;
	bool isDirShared(const string& directory);
	bool isFileShared(const TTHValue aTTH, const string& fileName);
	bool allowAddDir(const string& dir);
	string getReleaseDir(const string& aName);
	tstring getDirPath(const string& directory, bool validate = true);
	string getBloomStats();

	bool loadCache();

	StringPairList getDirectories(int refreshOptions) const noexcept;
	vector<pair<string, StringList>> getGroupedDirectories() const noexcept;
	static bool checkType(const string& aString, int aType);
	MemoryInputStream* generatePartialList(const string& dir, bool recurse, bool isInSharingHub, const HintedUser& aUser, const string& userSID);
	MemoryInputStream* generateTTHList(const string& dir, bool recurse, bool isInSharingHub, const HintedUser& aUser);
	MemoryInputStream* getTree(const string& virtualFile, const HintedUser& aUser, const string& userSID) const;

	AdcCommand getFileInfo(const string& aFile, Client* client);

	int64_t getShareSize(Client* client = NULL) const noexcept;
	int64_t getShareSize(const string& realPath) const noexcept;

	size_t getSharedFiles() const noexcept;

	string getShareSizeString(Client* client = NULL) { return Util::toString(getShareSize(client)); }
	string getShareSizeString(const string& aDir) const { return Util::toString(getShareSize(aDir)); }
	
	void getBloom(ByteVector& v, size_t k, size_t m, size_t h) const;

	SearchManager::TypeModes getType(const string& fileName) noexcept;

	string validateVirtual(const string& /*aVirt*/) const noexcept;
	bool hasVirtual(const string& name) const noexcept;

	void addHits(uint32_t aHits) {
		hits += aHits;
	}

	string generateOwnList(const string& hubUrl = Util::emptyString) {
		Client* c = NULL;
		if(!hubUrl.empty())
			c = ClientManager::getInstance()->findClient(hubUrl);

		FileList* fl = generateXmlList(c, true);
		return fl->getBZXmlFile();
	}

	bool isTTHShared(const TTHValue& tth) {
		RLock l(cs);
		for(auto i = directories.begin(); i != directories.end(); ++i) {
			if(i->second->getRoot()->tthIndex.find(const_cast<TTHValue*>(&tth)) != i->second->getRoot()->tthIndex.end())
				return true;
		}
	}

	void getRealPaths(const string& path, StringList& ret);

	void LockRead() noexcept { cs.lock_shared(); }
	void unLockRead() noexcept { cs.unlock_shared(); }

	string getRealPath(const TTHValue& root);

	enum { 
		REFRESH_STARTED = 0,
		REFRESH_PATH_NOT_FOUND = 1,
		REFRESH_IN_PROGRESS = 2
	};
	enum {
		REFRESH_ALL = 0x01,
		REFRESH_DIRECTORY = 0x02,
		REFRESH_BLOCKING = 0x04,
		REFRESH_UPDATE = 0x08,
		REFRESH_INCOMING = 0x10
	};

	GETSET(size_t, hits, Hits);
	GETSET(int64_t, sharedSize, SharedSize);

	//tempShares
	struct TempShareInfo {
		TempShareInfo(const string& aKey, const string& aPath, int64_t aSize) : key(aKey), path(aPath), size(aSize) { }
		
		string key; //CID or hubUrl
		string path; //filepath
		int64_t size; //filesize
	};

	typedef unordered_multimap<TTHValue, TempShareInfo> TempShareMap;
	TempShareMap tempShares;
	CriticalSection tScs;
	bool addTempShare(const string& aKey, TTHValue& tth, const string& filePath, int64_t aSize, bool adcHub);
	bool hasTempShares() { Lock l(tScs); return !tempShares.empty(); }
	TempShareMap getTempShares() { Lock l(tScs); return tempShares; }
	void removeTempShare(const string& aKey, TTHValue& tth);
	TempShareInfo findTempShare(const string& aKey, const string& virtualFile);
	//tempShares end

private:
	struct AdcSearch;
	class Directory : public intrusive_ptr_base<Directory>, boost::noncopyable {
	public:
		typedef boost::intrusive_ptr<Directory> Ptr;
		typedef unordered_map<string, Ptr, noCaseStringHash, noCaseStringEq> Map;
		typedef Map::iterator MapIter;

		struct File {
			struct StringComp {
				StringComp(const string& s) : a(s) { }
				bool operator()(const File& b) const { return stricmp(a, b.getName()) == 0; }
				const string& a;
			private:
				StringComp& operator=(const StringComp&);
			};
			struct FileLess {
				bool operator()(const File& a, const File& b) const { return (stricmp(a.getName(), b.getName()) < 0); }
			};
			typedef set<File, FileLess> Set;

			File() : size(0), parent(0) { }
			File(const string& aName, int64_t aSize, Directory::Ptr aParent, const TTHValue& aRoot) : 
				name(aName), tth(aRoot), size(aSize), parent(aParent.get()) { }
			File(const File& rhs) : 
				name(rhs.getName()), tth(rhs.getTTH()), size(rhs.getSize()), parent(rhs.getParent()) { }

			~File() { }

			File& operator=(const File& rhs) {
				name = rhs.name; size = rhs.size; parent = rhs.parent; tth = rhs.tth;
				return *this;
			}

			bool operator==(const File& rhs) const {
				return getParent() == rhs.getParent() && (stricmp(getName(), rhs.getName()) == 0);
			}
		
			string getADCPath() const { return parent->getADCPath() + name; }
			string getFullName() const { return parent->getFullName() + name; }
			string getRealPath(bool validate = true) const { return parent->getRealPath(name, validate); }

			GETSET(TTHValue, tth, TTH);
			GETSET(string, name, Name);
			GETSET(int64_t, size, Size);
			GETSET(Directory*, parent, Parent);

		};

		class RootDirectory  {
			public:
				RootDirectory(const string& aRootPath) : path(aRootPath) { }
				typedef unordered_multimap<TTHValue*, Directory::File::Set::const_iterator> HashFileMap;
				typedef HashFileMap::const_iterator HashFileIter;

				HashFileMap tthIndex;
				GETSET(string, path, Path);
		
				~RootDirectory() { }
		};

		Map directories;
		File::Set files;

		static Ptr create(const string& aName, const Ptr& aParent, uint32_t&& aLastWrite, RootDirectory* aRoot = nullptr) {
			auto Ptr(new Directory(aName, aParent, aLastWrite, aRoot));
			if (aParent)
				aParent->directories[aName] = Ptr;
			return Ptr;
		}

		bool hasType(uint32_t type) const noexcept {
			return ( (type == SearchManager::TYPE_ANY) || (fileTypes & (1 << type)) );
		}
		void addType(uint32_t type) noexcept;

		string getADCPath() const noexcept;
		string getFullName() const noexcept; 
		string getRealPath(const std::string& path, bool validate = true) const;
		
		RootDirectory* findRoot() { 
			if(getParent()) 
				return getParent()->findRoot();
			else
				return root;
		}

		void increaseSize(uint64_t aSize);
		void decreaseSize(uint64_t aSize);
		void resetSize() { size = 0; }
		int64_t getSize() { return size; };
		size_t countFiles() const noexcept; //ApexDC

		void search(SearchResultList& aResults, StringSearch::List& aStrings, int aSearchType, int64_t aSize, int aFileType, Client* aClient, StringList::size_type maxResults) const noexcept;
		void search(SearchResultList& aResults, AdcSearch& aStrings, StringList::size_type maxResults) const noexcept;
		void findDirsRE(bool remove);

		void toXml(SimpleXML& aXml, bool fullList);
		void toTTHList(OutputStream& tthList, string& tmp2, bool recursive);
		void filesToXml(SimpleXML& aXml) const;
		//for filelist caching
		void toXmlList(OutputStream& xmlFile, const string& path, string& indent);

		File::Set::const_iterator findFile(const string& aFile) const { return find_if(files.begin(), files.end(), Directory::File::StringComp(aFile)); }

		string find(const string& dir, bool validateDir);

		GETSET(uint32_t, lastWrite, LastWrite);
		GETSET(string, name, Name);
		GETSET(Directory*, parent, Parent);
		GETSET(RootDirectory*, root, Root);

		Directory(const string& aName, const Ptr& aParent, uint32_t aLastWrite, RootDirectory* root = nullptr);
		~Directory() { 
			if(root)
				delete root;
		}
		
	private:
		friend void intrusive_ptr_release(intrusive_ptr_base<Directory>*);
		/** Set of flags that say which SearchManager::TYPE_* a directory contains */
		uint32_t fileTypes;
		
		int64_t size;
	};
	
	friend class Directory;
	friend struct ShareLoader;
	friend class FileList;

	friend class Singleton<ShareManager>;
	
	ShareManager();
	~ShareManager();
	
	struct AdcSearch {
		AdcSearch(const StringList& params);

		bool isExcluded(const string& str);
		bool hasExt(const string& name);

		StringSearch::List* include;
		StringSearch::List includeX;
		StringSearch::List exclude;
		StringList ext;
		StringList noExt;

		int64_t gt;
		int64_t lt;

		TTHValue root;
		bool hasRoot;

		bool isDirectory;
	};

	/*
	A Class that holds info on Hub spesific Filelist,
	a Full FileList that contains all like it did before is constructed with sharemanager instance, and then updated like before,
	this means that we should allways have FileListALL, other lists are just extra.
	Now this would be really simple if just used recursive Locks in sharemanager, to protect everything at once.
	BUT i dont want freezes and lockups so lets make it a bit more complex :) 
	..*/
	class FileList {
		public:
			FileList(const string& name) : Name(name), xmlDirty(true), forceXmlRefresh(true), lastxmlUpdate(0), listN(0) { }
			string Name;
		private:
			GETSET(int64_t, xmllistlen, xmlListLen);
			GETSET(TTHValue, xmlroot, xmlRoot);
			GETSET(int64_t, bzxmllistlen, bzXmlListLen);
			GETSET(TTHValue, bzxmlroot, bzXmlRoot);
			GETSET(uint64_t, lastxmlUpdate, lastXmlUpdate);
			GETSET(string, bzXmlFile, BZXmlFile);
			unique_ptr<File> bzXmlRef;
			int listN;
			bool xmlDirty;
			bool forceXmlRefresh; /// bypass the 15-minutes guard

	};
	//or just save the filelist in Client? might be there for nothing in most cases tho.
	typedef unordered_map<string, FileList*> FileListMap;
	FileListMap fileLists;

	FileList* generateXmlList(Client* client, bool forced = false);
	void createFileList(Client* client, FileList* fl, const string& flname, bool forced);
	FileList* getFileList(Client* client) const;

	bool isHubExcluded(const string& sharepath, const Client* client) const;
	bool isExcluded(const string& sharepath, const ClientList& clients) const;

	void saveXmlList(bool verbose = false);	//for filelist caching

	bool ShareCacheDirty;
	bool aShutdown;

	PME RAR_regexp;
	
	atomic_flag refreshing;
	atomic_flag GeneratingFULLXmlList;

	uint64_t lastFullUpdate;
	uint64_t lastIncomingUpdate;
	uint64_t lastSave;
	uint32_t findLastWrite(const string& aName) const;
	
	//caching the share size so we dont need to loop tthindex everytime
	bool xml_saving;

	mutable SharedMutex cs;  // NON-recursive mutex BE Aware!!
	mutable CriticalSection dirnamelist;

	int allSearches, stoppedSearches;
	int refreshOptions;
	
	/* Releases */
	StringList dirNameList;
	void addReleaseDir(const string& aName);
	void deleteReleaseDir(const string& aName);
	void sortReleaseList();

	/*
	multimap to allow multiple same key values, needed to return from some functions.
	*/
	typedef std::multimap<string, Directory::Ptr> DirMultiMap; 

	//list to return multiple directory item pointers
	typedef std::vector<Directory::Ptr> Dirs;

	/*Map of root directory items mapped to realpath*/
	typedef std::unordered_map<string, Directory::Ptr, noCaseStringHash, noCaseStringEq> DirMap; 
	DirMap directories;

	/** Map real name to virtual name - multiple real names may be mapped to a single virtual one */
	StringMap shares;

	BloomFilter<5> bloom;
	
	Directory::File::Set::const_iterator findFile(const string& virtualFile, const ClientList& clients) const;

	void buildTree(const string& aName, const Directory::Ptr& aDir, bool checkQueued = false);
	bool checkHidden(const string& aName) const;

	void rebuildIndices();
	void updateIndices(Directory& aDirectory, Directory::RootDirectory& root, bool first=true);
	void updateIndices(Directory& dir, const Directory::File::Set::iterator& i, Directory::RootDirectory& root);
	void cleanIndices(Directory::Ptr& dir);

	void onFileHashed(const string& fname, const TTHValue& root);
	
	StringList notShared;
	StringList incoming;
	StringList bundleDirs;
	StringList refreshPaths;

	Dirs getByVirtual(const string& virtualName, const ClientList& clients) const noexcept;
	DirMultiMap findVirtuals(const string& virtualPath, const ClientList& clients) const;
	string findRealRoot(const string& virtualRoot, const string& virtualLeaf) const;

	Directory::Ptr findDirectory(const string& fname, bool allowAdd, bool report);

	int run();

	// QueueManagerListener
	virtual void on(QueueManagerListener::BundleHashed, const string& path) noexcept;
	virtual void on(QueueManagerListener::FileHashed, const string& fname, const TTHValue& root) noexcept { onFileHashed(fname, root); }

	// SettingsManagerListener
	void on(SettingsManagerListener::Save, SimpleXML& xml) noexcept {
		save(xml);
	}
	void on(SettingsManagerListener::Load, SimpleXML& xml) noexcept {
		load(xml);
	}
	
	// TimerManagerListener
	void on(TimerManagerListener::Minute, uint64_t tick) noexcept;
	void load(SimpleXML& aXml);
	void save(SimpleXML& aXml);
	

/*This will only be used by the big sharing people probobly*/
class Worker: public Thread
{
public:
	Worker() { }
	 ~Worker() {}

private:
		int run() {
			ShareManager::getInstance()->saveXmlList();
			return 0;
		}
	};//worker end

friend class Worker;
Worker w;

}; //sharemanager end

} // namespace dcpp

#endif // !defined(SHARE_MANAGER_H)
