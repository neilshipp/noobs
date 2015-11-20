#include "multiimagewritethread.h"
#include "config.h"
#include "json.h"
#include "util.h"
#include "mbr.h"
#include <QDir>
#include <QFile>
#include <QDebug>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QTime>
#include <unistd.h>
#include <linux/fs.h>
#include <sys/ioctl.h>

MultiImageWriteThread::MultiImageWriteThread(QObject *parent) :
    QThread(parent), _extraSpacePerPartition(0), _part(6)
{
    QDir dir;

    if (!dir.exists("/mnt2"))
        dir.mkdir("/mnt2");
}

void MultiImageWriteThread::addImage(const QString &folder, const QString &flavour)
{
    _images.insert(folder, flavour);
}

void MultiImageWriteThread::run()
{
    /* Calculate space requirements */
    int totalnominalsize = 0, totaluncompressedsize = 0, numparts = 0, numexpandparts = 0, numext4expandparts = 0;
    int win10Fat = 0, win10Ntfs = 0;
    bool RiscOSworkaround = false;
    int startSector = getFileContents("/sys/class/block/mmcblk0p4/start").trimmed().toULongLong() + SETTINGS_PARTITION_SIZE + EBR_PARTITION_OFFSET;
    int availableMB = (sizeofSDCardInBlocks() - startSector)/2048;

    foreach (QString folder, _images.keys())
    {
        QVariantList partitions = Json::loadFromFile(folder+"/partitions.json").toMap().value("partitions").toList();
        if (partitions.isEmpty())
        {
            emit error(tr("partitions.json invalid"));
            return;
        }

        foreach (QVariant pv, partitions)
        {
            QVariantMap partition = pv.toMap();
            numparts++;
            if ( partition.value("want_maximised").toBool() )
                numexpandparts++;
            totalnominalsize += partition.value("partition_size_nominal").toInt();
            totaluncompressedsize += partition.value("uncompressed_tarball_size").toInt();
            if (partition.value("filesystem_type").toString() == "ext4")
            {
                totaluncompressedsize += /*0.035*/ 0.01 * totalnominalsize; /* overhead for file system meta data */
                if (partition.value("want_maximised").toBool())
                {
                    numext4expandparts++;
                }
            }

            if (nameMatchesWinIoT(folder))
            {
                /* Windows IoT partitions cannot be an extended partition.
                   Reserve space after the extended partition */
                int partitionSize = partition.value("partition_size_nominal").toInt() * 2048;
                if (partition.value("filesystem_type").toString() == "NTFS" ||
                    partition.value("filesystem_type").toString() == "ntfs")
                {
                    if (win10Ntfs != 0)
                    {
                        emit error(tr("WinIoT cannot have more than 1 NTFS partition."));
                        return;
                    }

                    win10Ntfs = partitionSize;
                }
                if (partition.value("filesystem_type").toString() == "FAT" ||
                    partition.value("filesystem_type").toString() == "fat")
                {
                    if (win10Fat != 0)
                    {
                        emit error(tr("WinIoT cannot have more than 1 FAT partition."));
                        return;
                    }

                    win10Fat = partitionSize;
                }
            }
        }

        if (nameMatchesRiscOS(folder))
        {
            /* Check the riscos_offset in os.json matches what we're expecting.
               In theory we shouldn't hit either of these errors because the invalid RISC_OS
               should have been filtered out already (not added to OS-list) in mainwindow.cpp */
            QVariantMap vos = Json::loadFromFile(folder+"/os.json").toMap();
            if (vos.contains(RISCOS_OFFSET_KEY))
            {
                int riscos_offset = vos.value(RISCOS_OFFSET_KEY).toInt();
                if (riscos_offset != RISCOS_OFFSET)
                {
                    emit error(tr("RISCOS cannot be installed. RISCOS offset value mismatch."));
                    return;
                }
            }
            else
            {
                emit error(tr("RISCOS cannot be installed. RISCOS offset value missing."));
                return;
            }
            if (startSector > RISCOS_SECTOR_OFFSET - EBR_PARTITION_OFFSET)
            {
                emit error(tr("RISCOS cannot be installed. Size of recovery partition too large."));
                return;
            }

            totalnominalsize += (RISCOS_SECTOR_OFFSET - startSector)/2048;
            RiscOSworkaround = true;
        }
    }

    /* 4 MB overhead per partition (logical partition table) */
    totalnominalsize += (numparts * 4);

    if (numexpandparts)
    {
        /* Extra spare space available for partitions that want to be expanded */
        _extraSpacePerPartition = (availableMB-totalnominalsize)/numexpandparts;
        /* Ext4 file system meta data overhead */
        //totaluncompressedsize += 0.035 * _extraSpacePerPartition * numext4expandparts;
    }

    emit parsedImagesize(qint64(totaluncompressedsize)*1024*1024);

    if (totalnominalsize > availableMB)
    {
        emit error(tr("Not enough disk space. Need %1 MB, got %2 MB").arg(QString::number(totalnominalsize), QString::number(availableMB)));
        return;
    }

    emit statusUpdate(tr("Clearing existing EBR"));
    clearEBR();

    emit statusUpdate(tr("Removing partions 2 and 3"));

    if (!sfdisk(3, 0, 0, "0") ||
        !sfdisk(2, 0, 0, "0"))
        return;

    if (win10Fat || win10Ntfs)
    {
        emit statusUpdate(tr("Reallocating space for Windows IoT"));

        /* Reserve the space at end of SD card */
        if (!reduceExtendedPartition(win10Fat + win10Ntfs))
            return;

        if (!sfdisk(2, sizeofSDCardInBlocks() - win10Fat - win10Ntfs, win10Fat, "c") ||
            !sfdisk(3, sizeofSDCardInBlocks() - win10Ntfs, win10Ntfs, "7"))
            return;
    }

    /* Install RiscOS first */
    if (RiscOSworkaround)
    {
        for (QMultiMap<QString,QString>::const_iterator iter = _images.constBegin(); iter != _images.constEnd(); iter++)
        {
            if (nameMatchesRiscOS(iter.key()))
            {
                if (!processImage(iter.key(), iter.value()))
                    return;

                _images.remove(iter.key());
                break;
            }
        }
    }

    /* Process each image */
    /* At this point no more calls to sfdisk can be done due to it complaining
       about invalid extended partitions created by addPartitionImage */

    for (QMultiMap<QString,QString>::const_iterator iter = _images.constBegin(); iter != _images.constEnd(); iter++)
    {
        if (!processImage(iter.key(), iter.value()))
            return;
    }

    emit statusUpdate(tr("Finish writing (sync)"));
    sync();
    emit completed();
}

bool MultiImageWriteThread::processImage(const QString &folder, const QString &flavour)
{
    QString os_name = (folder.split("/")).at(3);

    qDebug() << "Processing OS:" << os_name;

    int startSector = getFileContents("/sys/class/block/mmcblk0p4/start").trimmed().toULongLong();

    QVariantList partitions = Json::loadFromFile(folder+"/partitions.json").toMap().value("partitions").toList();
    QVariantList vpartitions;
    foreach (QVariant pv, partitions)
    {
        QVariantMap partition = pv.toMap();

        QByteArray fstype   = partition.value("filesystem_type").toByteArray();
        QByteArray mkfsopt  = partition.value("mkfs_options").toByteArray();
        QByteArray label = partition.value("label").toByteArray();
        QString tarball  = partition.value("tarball").toString();
        bool emptyfs     = partition.value("empty_fs", false).toBool();

        if (!emptyfs && tarball.isEmpty())
        {
            /* If no tarball URL is specified, we expect the tarball to reside in the folder and be named <label.tar.xz> */
            if (fstype == "raw" || fstype == "ntfs" || fstype == "NTFS")
                tarball = folder+"/"+label+".xz";
            else
                tarball = folder+"/"+label+".tar.xz";

            if (!QFile::exists(tarball))
            {
                emit error(tr("File '%1' does not exist").arg(tarball));
                return false;
            }
        }
        if (label.size() > 15)
        {
            label.clear();
        }
        else if (!isLabelAvailable(label))
        {
            for (int i=0; i<10; i++)
            {
                if (isLabelAvailable(label+QByteArray::number(i)))
                {
                    label = label+QByteArray::number(i);
                    break;
                }
            }
        }

        int partsizeMB = partition.value("partition_size_nominal").toInt();
        if (!partsizeMB)
        {
            emit error(tr("Nominal partition size not specified or zero"));
            return false;
        }

        if ( partition.value("want_maximised").toBool() )
            partsizeMB += _extraSpacePerPartition;

        QByteArray partdevice = "/dev/mmcblk0p"+QByteArray::number(_part);
        int partsizeSectors = partsizeMB * 2048;
        int parttype;
        int specialOffset = 0;

        if (fstype == "FAT" || fstype == "fat")
            parttype = 0x0c; /* FAT32 LBA */
        else if (fstype == "swap")
            parttype = 0x82;
        else if (fstype == "NTFS" || fstype == "ntfs")
            parttype = 0x07; /* NTFS */
        else
            parttype = 0x83; /* Linux native */

        if (nameMatchesRiscOS(folder) && (fstype == "FAT" || fstype == "fat"))
        {
            /* Let Risc OS start at known offset */
            specialOffset = RISCOS_SECTOR_OFFSET - startSector - EBR_PARTITION_OFFSET;
        }

        emit statusUpdate(tr("%1: Creating partition entry").arg(os_name));
        if (nameMatchesWinIoT(folder) && (fstype == "FAT" || fstype == "fat"))
        {
            /* Windows IoT uses primary partition 2, not extended partitions */
            partdevice = "/dev/mmcblk0p2";
            _part--;
        }
        else if (nameMatchesWinIoT(folder) && (fstype == "NTFS" || fstype == "ntfs"))
        {
            /* Windows IoT uses primary partition 3, not extended partitions */
            partdevice = "/dev/mmcblk0p3";
            _part--;
        }
        else
        {
            if (!addPartitionEntry(partsizeSectors, parttype, specialOffset))
                return false;
        }

        if (fstype == "raw" || fstype == "NTFS" || fstype == "ntfs")
        {
            emit statusUpdate(tr("%1: Writing OS image %2").arg(os_name, tarball));

            if (!emptyfs && !dd(tarball, partdevice))
                return false;
        }
        else
        {
            emit runningMKFS();
            emit statusUpdate(tr("%1: Creating filesystem (%2)").arg(os_name, QString(fstype)));
            if (!mkfs(partdevice, fstype, label, mkfsopt))
                return false;
            emit finishedMKFS();

            if (!emptyfs)
            {
                emit statusUpdate(tr("%1: Mounting file system").arg(os_name));
                if (QProcess::execute("mount "+partdevice+" /mnt2") != 0)
                {
                    emit error(tr("%1: Error mounting file system").arg(os_name));
                    return false;
                }

                if (tarball.startsWith("http"))
                    emit statusUpdate(tr("%1: Downloading and extracting filesystem %2").arg(os_name, tarball));
                else
                    emit statusUpdate(tr("%1: Extracting filesystem %2").arg(os_name, tarball));

                bool result = untar(tarball);

                if (QProcess::execute("umount /mnt2") !=0)
                {
                    emit error(tr("%1: Error unmounting file system").arg(os_name));
                    return false;
                }

                if (!result)
                    return false;
            }
        }

        vpartitions.append(partdevice);
        _part++;
    }

    QString firstPartition = vpartitions.at(0).toString();
    emit statusUpdate(tr("%1: Mounting FAT partition %2").arg(os_name, firstPartition));
    if (QProcess::execute("mount "+firstPartition+" /mnt2") != 0)
    {
        emit error(tr("%1: Error mounting file system %2").arg(os_name, firstPartition));
        return false;
    }

    emit statusUpdate(tr("%1: Creating os_config.json").arg(os_name));

    QString description = getDescription(folder, flavour);
    QSettings settings("/settings/noobs.conf", QSettings::IniFormat);
    int videomode = settings.value("display_mode", 0).toInt();
    QString language = settings.value("language", "en").toString();
    QString keyboard = settings.value("keyboard_layout", "gb").toString();

    QVariantMap vos = Json::loadFromFile(folder+"/os.json").toMap();
    QVariant releasedate = vos.value("release_date");

    QVariantMap qm;
    qm.insert("flavour", flavour);
    qm.insert("release_date", releasedate);
    qm.insert("imagefolder", folder);
    qm.insert("description", description);
    qm.insert("videomode", videomode);
    qm.insert("partitions", vpartitions);
    qm.insert("language", language);
    qm.insert("keyboard", keyboard);

    Json::saveToFile("/mnt2/os_config.json", qm);

    emit statusUpdate(tr("%1: Saving display mode to config.txt").arg(os_name));
    patchConfigTxt();

    /* Partition setup script can either reside in the image folder
     * or inside the boot partition tarball */
    QString postInstallScript = folder+"/partition_setup.sh";
    if (!QFile::exists(postInstallScript))
        postInstallScript = "/mnt2/partition_setup.sh";

    if (QFile::exists(postInstallScript))
    {
        emit statusUpdate(tr("%1: Running partition setup script").arg(os_name));
        QProcess proc;
        QProcessEnvironment env;
        QStringList args(postInstallScript);
        env.insert("PATH", "/bin:/usr/bin:/sbin:/usr/sbin");

        /* - Parameters to the partition-setup script are supplied both as
         *   command line parameters and set as environement variables
         * - Boot partition is mounted, working directory is set to its mnt folder
         *
         *  partition_setup.sh part1=/dev/mmcblk0p3 id1=LABEL=BOOT part2=/dev/mmcblk0p4
         *  id2=UUID=550e8400-e29b-41d4-a716-446655440000
         */
        for (int i=0, pcount = 1; i < vpartitions.length(); i++, pcount++)
        {
            QString part  = vpartitions.at(i).toString();
            QString nr    = QString::number(pcount);
            QString uuid  = getUUID(part);
            QString label = getLabel(part);
            QString id;
            if (!label.isEmpty())
                id = "LABEL="+label;
            else
                id = "UUID="+uuid;

            qDebug() << "part" << part << uuid << label;

            args << "part"+nr+"="+part << "id"+nr+"="+id;
            env.insert("part"+nr, part);
            env.insert("id"+nr, id);
        }

        qDebug() << "Executing: sh" << args;
        qDebug() << "Env:" << env.toStringList();
        proc.setProcessChannelMode(proc.MergedChannels);
        proc.setProcessEnvironment(env);
        proc.setWorkingDirectory("/mnt2");
        proc.start("/bin/sh", args);
        proc.waitForFinished(-1);
        qDebug() << proc.exitStatus();

        if (proc.exitCode() != 0)
        {
            emit error(tr("%1: Error executing partition setup script").arg(os_name)+"\n"+proc.readAll());
            return false;
        }
    }

    emit statusUpdate(tr("%1: Unmounting FAT partition").arg(os_name));
    if (QProcess::execute("umount /mnt2") != 0)
    {
        emit error(tr("%1: Error unmounting").arg(os_name));
    }

    /* Save information about installed operating systems in installed_os.json */
    QVariantMap ventry;
    ventry["name"]        = flavour;
    ventry["description"] = description;
    ventry["folder"]      = folder;
    ventry["release_date"]= releasedate;
    ventry["partitions"]  = vpartitions;
    if (vos.contains("bootable"))
        ventry["bootable"] = vos.value("bootable").toBool();
    QString iconfilename = folder+"/"+flavour+".png";
    iconfilename.replace(" ", "_");
    if (QFile::exists(iconfilename))
        ventry["icon"] = iconfilename;
    else if (QFile::exists(folder+"/icon.png"))
        ventry["icon"] = folder+"/icon.png";
    installed_os.append(ventry);

    QProcess::execute("mount -o remount,rw /settings");
    Json::saveToFile("/settings/installed_os.json", installed_os);
    QProcess::execute("mount -o remount,ro /settings");

    return true;
}

bool MultiImageWriteThread::reduceExtendedPartition(int size)
{
    int startOfExtended = getFileContents("/sys/class/block/mmcblk0p4/start").trimmed().toULongLong();
    int sizeOfExtended = sizeofSDCardInBlocks() - startOfExtended;

    if (size + SETTINGS_PARTITION_SIZE + EBR_PARTITION_OFFSET> sizeOfExtended)
    {
        emit error(tr("Error reallocating extended partition - partition too large"));
        return false;
    }

    /* Let sfdisk update the extended partition */
    if (!sfdisk(4, startOfExtended, sizeOfExtended - size, "05"))
        return false;

    return true;
}

bool MultiImageWriteThread::sfdisk(int part, int start, int size, const QByteArray &type)
{
    /* Unmount everything before modifying partition table */
    QProcess::execute("umount -r /mnt");
    QProcess::execute("umount -r /settings");

    QString cmd = QString("/sbin/sfdisk -uS /dev/mmcblk0 -N") + QString::number(part);
    QByteArray partition = QByteArray::number(start)+","+QByteArray::number(size)+","+type+"\n";                                       
    
    QProcess proc;
    proc.setProcessChannelMode(proc.MergedChannels);
    proc.start(cmd);
    proc.write(partition);
    proc.closeWriteChannel();
    proc.waitForFinished(-1);
    if (proc.exitCode() != 0)
    {
        emit error(tr("Error creating partition %1").arg(QString::number(part))+"\n"+proc.readAll());
        return false;
    }
    qDebug() << "sfdisk done, output:" << proc.readAll();
    
    QProcess::execute("/usr/sbin/partprobe");
    QThread::msleep(500);

    /* Remount */
    QProcess::execute("mount -o ro -t vfat /dev/mmcblk0p1 /mnt");
    QProcess::execute("mount -o ro -t ext4 " SETTINGS_PARTITION " /settings");

    return true;
}

void MultiImageWriteThread::clearEBR()
{
    /* Unmount everything before modifying partition table */
    QProcess::execute("umount -r /mnt");
    QProcess::execute("umount -r /settings");

    mbr_table ebr;
    int startOfExtended = getFileContents("/sys/class/block/mmcblk0p4/start").trimmed().toULongLong();

    /* Write out extended partition table with single settings partition */
    memset(&ebr, 0, sizeof ebr);
    ebr.part[0].starting_sector = EBR_PARTITION_OFFSET;
    ebr.part[0].nr_of_sectors = SETTINGS_PARTITION_SIZE;
    ebr.part[0].id = 0x83;
    ebr.signature[0] = 0x55;
    ebr.signature[1] = 0xAA;
    QFile f("/dev/mmcblk0");
    f.open(f.ReadWrite);
    f.seek(qint64(startOfExtended)*512);
    f.write((char *) &ebr, sizeof(ebr));
    // Tell Linux to re-read the partition table
    f.flush();
    ioctl(f.handle(), BLKRRPART);
    f.close();
    QThread::msleep(500);

    /* Remount */
    QProcess::execute("mount -o ro -t vfat /dev/mmcblk0p1 /mnt");
    QProcess::execute("mount -t ext4 " SETTINGS_PARTITION " /settings");
}

bool MultiImageWriteThread::addPartitionEntry(int sizeInSectors, int type, int specialOffset)
{
    /* Unmount everything before modifying partition table */
    QProcess::execute("umount -r /mnt");
    QProcess::execute("umount -r /settings");

    unsigned int startOfExtended = getFileContents("/sys/class/block/mmcblk0p4/start").trimmed().toULongLong();
    unsigned int offsetInSectors = 0;
    mbr_table ebr;
    QFile f("/dev/mmcblk0");
    if (!f.open(f.ReadWrite))
    {
        emit error(tr("Error opening /dev/mmcblk0 for writing"));
        return false;
    }

    /* Find last EBR */
    do
    {
        f.seek(qint64(startOfExtended+offsetInSectors)*512);
        f.read((char *) &ebr, sizeof(ebr));

        if (ebr.part[1].starting_sector)
        {
            if (ebr.part[1].starting_sector > offsetInSectors)
            {
                offsetInSectors = ebr.part[1].starting_sector;
            }
            else
            {
                emit error(tr("Internal error in partitioning"));
                return false;
            }
        }
    } while (ebr.part[1].starting_sector != 0);

    if (ebr.part[0].starting_sector)
    {
        /* Add reference to new EBR to old last EBR */
        ebr.part[1].starting_sector = offsetInSectors + ebr.part[0].starting_sector + ebr.part[0].nr_of_sectors;
        ebr.part[1].nr_of_sectors = sizeInSectors + EBR_PARTITION_OFFSET;
        ebr.part[1].id = 0x0F;

        if (specialOffset)
        {
            if (ebr.part[1].starting_sector > specialOffset)
            {
                emit error(tr("Internal error in RiscOS partitioning"));
                return false;
            }
            ebr.part[1].starting_sector = specialOffset;
        }

        f.seek(qint64(startOfExtended+offsetInSectors)*512);
        f.write((char *) &ebr, sizeof(ebr));
        offsetInSectors = ebr.part[1].starting_sector;
        qDebug() << "add tail";
    }

    memset(&ebr, 0, sizeof ebr);
    ebr.signature[0] = 0x55;
    ebr.signature[1] = 0xAA;

    if (specialOffset)
    {
        ebr.part[0].starting_sector = EBR_PARTITION_OFFSET + specialOffset - offsetInSectors;
    }
    else
    {
        ebr.part[0].starting_sector = ((((startOfExtended + offsetInSectors + EBR_PARTITION_OFFSET) + 6144) / 8192) * 8192) - (startOfExtended + offsetInSectors);
    }

    ebr.part[0].nr_of_sectors = sizeInSectors;
    ebr.part[0].id = type;
    f.seek(qint64(startOfExtended+offsetInSectors)*512);
    f.write((char *) &ebr, sizeof(ebr));
    f.flush();
    /* Tell Linux to re-read the partition table */
    ioctl(f.handle(), BLKRRPART);
    f.close();

    /* Call partprobe just in case. BLKRRPART should be enough though */
    QProcess::execute("/usr/sbin/partprobe");
    QThread::msleep(500);

    /* Remount */
    QProcess::execute("mount -o ro -t vfat /dev/mmcblk0p1 /mnt");
    QProcess::execute("mount -t ext4 " SETTINGS_PARTITION " /settings");

    return true;
}

bool MultiImageWriteThread::mkfs(const QByteArray &device, const QByteArray &fstype, const QByteArray &label, const QByteArray &mkfsopt)
{
    QString cmd;

    if (fstype == "fat" || fstype == "FAT")
    {
        cmd = "/sbin/mkfs.fat ";
        if (!label.isEmpty())
        {
            cmd += "-n "+label+" ";
        }
    }
    else if (fstype == "ext4")
    {
        cmd = "/usr/sbin/mkfs.ext4 ";
        if (!label.isEmpty())
        {
            cmd += "-L "+label+" ";
        }
    }

    if (!mkfsopt.isEmpty())
        cmd += mkfsopt+" ";

    cmd += device;

    qDebug() << "Executing:" << cmd;
    QProcess p;
    p.setProcessChannelMode(p.MergedChannels);
    p.start(cmd);
    p.closeWriteChannel();
    p.waitForFinished(-1);

    if (p.exitCode() != 0)
    {
        emit error(tr("Error creating file system")+"\n"+p.readAll());
        return false;
    }

    return true;
}

bool MultiImageWriteThread::isLabelAvailable(const QByteArray &label)
{
    return (QProcess::execute("/sbin/findfs LABEL="+label) != 0);
}

bool MultiImageWriteThread::untar(const QString &tarball)
{
    QString cmd = "sh -o pipefail -c \"";

    if (tarball.startsWith("http:"))
        cmd += "wget --no-verbose --tries=inf -O- "+tarball+" | ";

    if (tarball.endsWith(".gz"))
    {
        cmd += "gzip -dc";
    }
    else if (tarball.endsWith(".xz"))
    {
        cmd += "xz -dc";
    }
    else if (tarball.endsWith(".bz2"))
    {
        cmd += "bzip2 -dc";
    }
    else if (tarball.endsWith(".lzo"))
    {
        cmd += "lzop -dc";
    }
    else if (tarball.endsWith(".zip"))
    {
        /* Note: the image must be the only file inside the .zip */
        cmd += "unzip -p";
    }
    else
    {
        emit error(tr("Unknown compression format file extension. Expecting .lzo, .gz, .xz, .bz2 or .zip\n%1").arg(tarball));
        return false;
    }
    if (!tarball.startsWith("http:"))
    {

        cmd += " "+tarball;
    }
    cmd += " | tar x -C /mnt2 ";
    cmd += "\"";

    QTime t1;
    t1.start();
    qDebug() << "Executing:" << cmd;

    QProcess p;
    p.setProcessChannelMode(p.MergedChannels);
    p.start(cmd);
    p.closeWriteChannel();
    p.waitForFinished(-1);

    if (p.exitCode() != 0)
    {
        QByteArray msg = p.readAll();
        qDebug() << msg;
        emit error(tr("Error downloading or extracting tarball")+"\n"+msg);
        return false;
    }
    qDebug() << "finished writing filesystem in" << (t1.elapsed()/1000.0) << "seconds";

    return true;
}

bool MultiImageWriteThread::dd(const QString &imagePath, const QString &device)
{
    QString cmd = "sh -o pipefail -c \"";

    if (imagePath.startsWith("http:"))
        cmd += "wget --no-verbose --tries=inf -O- "+imagePath+" | ";

    if (imagePath.endsWith(".gz"))
    {
        cmd += "gzip -dc";
    }
    else if (imagePath.endsWith(".xz"))
    {
        cmd += "xz -dc";
    }
    else if (imagePath.endsWith(".bz2"))
    {
        cmd += "bzip2 -dc";
    }
    else if (imagePath.endsWith(".lzo"))
    {
        cmd += "lzop -dc";
    }
    else if (imagePath.endsWith(".zip"))
    {
        /* Note: the image must be the only file inside the .zip */
        cmd += "unzip -p";
    }
    else
    {
        emit error(tr("Unknown compression format file extension. Expecting .lzo, .gz, .xz, .bz2 or .zip\n%1 %2").arg(imagePath,device));
        return false;
    }

    if (!imagePath.startsWith("http:"))
        cmd += " "+imagePath;

    cmd += " | dd of="+device+" conv=fsync obs=4M\"";

    QTime t1;
    t1.start();
    qDebug() << "Executing:" << cmd;

    QProcess p;
    p.setProcessChannelMode(p.MergedChannels);
    p.start(cmd);
    p.closeWriteChannel();
    p.waitForFinished(-1);

    if (p.exitCode() != 0)
    {
        emit error(tr("Error downloading or writing OS to SD card")+"\n"+p.readAll());
        return false;
    }
    qDebug() << "finished writing filesystem in" << (t1.elapsed()/1000.0) << "seconds";

    return true;
}

void MultiImageWriteThread::patchConfigTxt()
{

        QSettings settings("/settings/noobs.conf", QSettings::IniFormat);
        int videomode = settings.value("display_mode", 0).toInt();

        QByteArray dispOptions;

        switch (videomode)
        {
        case 0: /* HDMI PREFERRED */
            dispOptions = "hdmi_force_hotplug=1\r\nconfig_hdmi_boost=4\r\noverscan_left=24\r\noverscan_right=24\r\noverscan_top=16\r\noverscan_bottom=16\r\ndisable_overscan=0\r\n";
            break;
        case 1: /* HDMI VGA */
            dispOptions = "hdmi_ignore_edid=0xa5000080\r\nhdmi_force_hotplug=1\r\nconfig_hdmi_boost=4\r\nhdmi_group=2\r\nhdmi_mode=4\r\n";
            break;
        case 2: /* PAL */
            dispOptions = "hdmi_ignore_hotplug=1\r\nsdtv_mode=2\r\n";
            break;
        case 3: /* NTSC */
            dispOptions = "hdmi_ignore_hotplug=1\r\nsdtv_mode=0\r\n";
            break;
        }


        QFile f("/mnt2/config.txt");
        f.open(f.Append);
        f.write("\r\n# NOOBS Auto-generated Settings:\r\n"+dispOptions);
        f.close();

}

QByteArray MultiImageWriteThread::getLabel(const QString part)
{
    QByteArray result;
    QProcess p;
    p.start("/sbin/blkid -s LABEL -o value "+part);
    p.waitForFinished();

    if (p.exitCode() == 0)
        result = p.readAll().trimmed();

    return result;
}

QByteArray MultiImageWriteThread::getUUID(const QString part)
{
    QByteArray result;
    QProcess p;
    p.start("/sbin/blkid -s UUID -o value "+part);
    p.waitForFinished();

    if (p.exitCode() == 0)
        result = p.readAll().trimmed();

    return result;
}

QString MultiImageWriteThread::getDescription(const QString &folder, const QString &flavour)
{
    if (QFile::exists(folder+"/flavours.json"))
    {
        QVariantMap v = Json::loadFromFile(folder+"/flavours.json").toMap();
        QVariantList fl = v.value("flavours").toList();

        foreach (QVariant f, fl)
        {
            QVariantMap fm  = f.toMap();
            if (fm.value("name").toString() == flavour)
            {
                return fm.value("description").toString();
            }
        }
    }
    else if (QFile::exists(folder+"/os.json"))
    {
        QVariantMap v = Json::loadFromFile(folder+"/os.json").toMap();
        return v.value("description").toString();
    }

    return "";
}

