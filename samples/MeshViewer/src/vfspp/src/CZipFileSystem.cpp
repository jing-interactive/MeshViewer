//
//  CZipFileSystem.cpp
//  vfspp
//
//  Created by Yevgeniy Logachev on 6/23/16.
//
//

#include "CZipFileSystem.h"
#include <fstream>
#include "CZipFile.h"
#include "CStringUtilsVFS.h"

using namespace vfspp;

// *****************************************************************************
// Constants
// *****************************************************************************

std::unordered_map<std::string, CZipPtr> CZipFileSystem::s_OpenedZips;

// *****************************************************************************
// Public Methods
// *****************************************************************************

CZipFileSystem::CZipFileSystem(const std::string& zipPath, const std::string& basePath)
: m_ZipPath(zipPath)
, m_basePath(basePath)
, m_IsInitialized(false)
{
}

CZipFileSystem::~CZipFileSystem()
{

}

void CZipFileSystem::Initialize()
{
    if (m_IsInitialized)
    {
        return;
    }
    
    std::lock_guard<decltype(m_Mutex)> lock(m_Mutex);
    m_Zip = s_OpenedZips[m_ZipPath];
    if (!m_Zip) {
        m_Zip.reset(new CZip(m_ZipPath));
        s_OpenedZips[m_ZipPath] = m_Zip;
    }
    m_IsInitialized = true;
}

void CZipFileSystem::Shutdown()
{
    std::lock_guard<decltype(m_Mutex)> lock(m_Mutex);
    m_Zip = nullptr;
    if (s_OpenedZips[m_ZipPath].use_count() == 1) {
        s_OpenedZips.erase(m_ZipPath);
    }
    m_FileList.clear();
    m_IsInitialized = false;
}


bool CZipFileSystem::IsInitialized() const
{
    return m_IsInitialized;
}


const std::string& CZipFileSystem::basePath() const
{
    return m_basePath;
}


const IFileSystem::TFileList& CZipFileSystem::FileList() const
{
    return m_FileList;
}


bool CZipFileSystem::isReadOnly() const
{
    if (!IsInitialized())
    {
        return true;
    }
    
    return m_Zip->isReadOnly();
}


IFilePtr CZipFileSystem::openFile(const CFileInfo& filePath, int mode)
{
    CFileInfo fileInfo(basePath(), filePath.absolutePath(), false);
    IFilePtr file = FindFile(fileInfo);
    bool isExists = (file != nullptr);
    if (!isExists)
    {        
        file.reset(new CZipFile(fileInfo, m_Zip));
    }
    file->Open(mode);
    
    if (!isExists && file->IsOpened())
    {
        m_FileList.insert(file);
    }
    
    return file;
}


void CZipFileSystem::closeFile(IFilePtr file)
{
    if (file)
    {
        file->Close();
    }
}


bool CZipFileSystem::createFile(const CFileInfo& filePath)
{
    bool result = false;
    if (!isFileExists(filePath))
    {
        IFilePtr file = openFile(filePath, IFile::ReadWrite);
        if (file)
        {
            result = true;
            file->Close();
        }
    }
    else
    {
        result = true;
    }
    
    return result;
}


bool CZipFileSystem::removeFile(const CFileInfo& filePath)
{
    return false; // TODO: Filesystem, temporally not suppoted
}


bool CZipFileSystem::copyFile(const CFileInfo& src, const CFileInfo& dest)
{
    bool result = false;
    if (!isReadOnly())
    {
        CZipFilePtr srcFile = std::static_pointer_cast<CZipFile>(FindFile(src));
        CZipFilePtr dstFile = std::static_pointer_cast<CZipFile>(openFile(dest, IFile::Out));
        
        if (srcFile && dstFile)
        {
            dstFile->m_Data.assign(srcFile->m_Data.begin(), srcFile->m_Data.end());
            dstFile->Close();
            
            result = true;
        }
    }
    
    return result;
}


bool CZipFileSystem::RenameFile(const CFileInfo& src, const CFileInfo& dest)
{
    return false; // TODO: Filesystem, temporally not suppoted
}


bool CZipFileSystem::isFileExists(const CFileInfo& filePath) const
{
    return (FindFile(basePath() + filePath.absolutePath()) != nullptr);
}


bool CZipFileSystem::IsFile(const CFileInfo& filePath) const
{
    IFilePtr file = FindFile(filePath);
    if (file)
    {
        return !file->FileInfo().IsDir();
    }
    
    return false;
}


bool CZipFileSystem::IsDir(const CFileInfo& dirPath) const
{
    IFilePtr file = FindFile(dirPath);
    if (file)
    {
        return file->FileInfo().IsDir();
    }
    
    return false;
}

// *****************************************************************************
// Protected Methods
// *****************************************************************************

// *****************************************************************************
// Private Methods
// *****************************************************************************

IFilePtr CZipFileSystem::FindFile(const CFileInfo& fileInfo) const
{
    TFileList::const_iterator it = std::find_if(m_FileList.begin(), m_FileList.end(), [&](IFilePtr file)
    {
        return file->FileInfo() == fileInfo;
    });
    
    if (it != m_FileList.end())
    {
        return *it;
    }
    
    return nullptr;
}
