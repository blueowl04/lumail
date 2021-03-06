/*
 * maildir.cc - Wrapper for a collection of messages.
 *
 * This file is part of lumail - http://lumail.org/
 *
 * Copyright (c) 2015 by Steve Kemp.  All rights reserved.
 *
 **
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 dated June, 1991, or (at your
 * option) any later version.
 *
 * On Debian GNU/Linux systems, the complete text of version 2 of the GNU
 * General Public License can be found in `/usr/share/common-licenses/GPL-2'
 */


#include <algorithm>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>


#include <gmime/gmime.h>


#include "directory.h"
#include "file.h"
#include "imap_proxy.h"
#include "maildir.h"
#include "message.h"
#include "util.h"


/*
 * Constructor.  Create an object to encapsulate the given path.
 */
CMaildir::CMaildir(const std::string name, bool is_local)
{
    m_path = name;

    if (is_local)
        m_imap = false;
    else
        m_imap = true;

    /*
     * Default cache-time.
     */
    m_modified = -1;
}


/*
 * Return the path we represent - NOTE: This might be a local
 * maildir-location, or a remote IMAP path.
 *
 * Use "is_imap" or "is_maildir" to tell the difference.
 */
std::string CMaildir::path()
{
    return (m_path);
}


/*
 * Destructor.
 */
CMaildir::~CMaildir()
{
}


/**
 * Is this maildir a local one?
 */
bool CMaildir::is_maildir()
{
    return (! m_imap);
}


/*
 * Is this maildir an IMAP path?
 */
bool CMaildir::is_imap()
{
    return (m_imap);

}


/*
 * The number of new messages for this maildir.
 */
int CMaildir::unread_messages()
{
    update_cache();
    return (m_unread);
}


/*
 * The total number of messages for this maildir.
 */
int CMaildir::total_messages()
{
    update_cache();
    return (m_total);
}



/*
 * Update the cached total/unread message counts.
 */
void CMaildir::update_cache()
{
    if (m_imap)
        return;

    /*
     * If the cached date isn't different then we need do nothing.
     */
    time_t last_mod = last_modified();

    if (last_mod == m_modified)
        return;

    /*
     * Otherwise update the last modified time.
     */
    m_modified = last_mod;

    /*
     * Get all messages, and update the total
     */
    CMessageList all = getMessages();
    m_total = all.size();


    /*
      * Now update the unread count.
      */
    m_unread = 0;

    for (std::shared_ptr < CMessage > message : all)
    {
        if (message->is_new())
            m_unread++;
    }
}

/*
 * Return the last modified time for this Maildir.
 */
time_t CMaildir::last_modified()
{

    if (m_imap)
    {
        return (m_modified);
    }

    time_t last = 0;
    struct stat st_buf;

    std::string p = path();

    /*
     * The two directories we care about: new/ + cur/
     */
    std::vector < std::string > dirs;
    dirs.push_back(p + "/cur");
    dirs.push_back(p + "/new");

    /*
     * See which was the most recently modified.
     */
    for (std::string dir : dirs)
    {
        /*
         * If we can stat() the dir and it is more recent
         * than the current value - update it.
         */
        if (!stat(dir.c_str(), &st_buf))
            if (st_buf.st_mtime > last)
                last = st_buf.st_mtime;
    }

    return (last);
}

/*
 * Get each messages in the folder.
 *
 * These are heap-allocated and will be persistent until the folder
 * selection is changed.
 *
 * The return value is *all possible messages*, no attention to `index_limit`
 * is paid.
 *
 */
CMessageList CMaildir::getMessages()
{
    CMessageList result;

    /*
     * Directories we search.
     */
    std::vector < std::string > dirs;
    dirs.push_back(m_path + "/cur/");
    dirs.push_back(m_path + "/new/");

    /*
     * For each directory.
     */
    for (std::string path : dirs)
    {

        /*
         * Get the entries in the directory
         */
        std::vector<std::string> entries = CDirectory::entries(path);

        /*
         * For each one - if it isn't a dirrectory create a message
         * using the path.
         *
         * This test is required because `CDirectory::entries` will return
         * both the prefix, and the children "." + "..".
         */
        for (std::string file : entries)
        {
            if (! CFile::is_directory(file))
            {
                std::shared_ptr < CMessage > t = std::shared_ptr < CMessage > (new CMessage(std::string(file)));
                result.push_back(t);
            }
        }
    }

    return result;
}


/*
 * Save the given message in this maildir.
 *
 * If this message is stored on a remote IMAP-server we handle
 * that specially.
 */
bool CMaildir::saveMessage(std::shared_ptr <CMessage > msg)
{
    /*
     * If we were created by IMAP then our folder will have
     * the m_imap flag set.
     *
     * Otherwise we'll not, but we can tell that we're non-local
     * because the first character of the path will not contain
     * the "/" character.
     *
     */
    if ((m_imap) || ((m_path.empty() == false) && (m_path.at(0) != '/')))
    {
        /*
         * IMAP SAVE
         */

        /*
         * Get the message path.
         */
        std::string msg_path = msg->path();

        /*
         * Get the folder-name we're saving to.
         */
        std::string folder = m_path;

        /*
         * Build up the string for the domain-socket helper.
         */
        std::string cmd = "save_message " + msg_path + " " + folder + "\n";

        /*
         * Get the output.
         */
        CIMAPProxy *proxy = CIMAPProxy::instance();
        std::string out  = proxy->read_imap_output(cmd);

        return (true);
    }
    else
    {
        std::string path = generate_filename(false);

        return (CFile::copy(msg->path(), path));
    }
}

/*
 * Generate a filename for saving a message into.
 */
std::string CMaildir::generate_filename(bool is_new)
{
    /*
     * Ensure the path to our maildir is a maildir
     */
    std::string tmp = path();
    std::string path = tmp;

    if (! CFile::is_maildir(path))
        return "";

    /*
     * Generate the path.
     */
    if (is_new)
        path += "/new/";
    else
        path += "/cur/";

    /*
     * Filename is: $time.xxx.$hostname.
     */
    char host[1024] = {'\0'};
    gethostname(host, sizeof(host) - 1);
    std::string hostname(host);

    /*
     * Loop until we found a file that is unique.
     */
    while (true)
    {
        /*
         * Convert the seconds past the epoch to a string.
         */
        time_t current_time = time(NULL);
        std::stringstream ss;
        ss << current_time;
        std::string since_epoch = ss.str();

        std::string file = since_epoch;
        file += ".";
        file += hostname;

        /*
         * Random number.
         */
        int r = rand() % 1000;
        ss << r;
        file += ss.str();

        /*
         * Generate the temporary file.
         */
        file += ":2";

        if (is_new)
            file += ",N";
        else
            file += ",S";

        if (! CFile::exists(path  + file))
            return (path + file);
    }
}


/*
 * Bump the modification-time of this maildir artificially
 * which is used solely for IMAP-based messages.
 */
void CMaildir::bump_mtime()
{
    m_modified += 1;
}
