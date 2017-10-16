// WorkerBrokerage - Manage worker discovery
//------------------------------------------------------------------------------

// Includes
//------------------------------------------------------------------------------
#include "WorkerBrokerage.h"

// FBuild
#include "Tools/FBuild/FBuildCore/Protocol/Protocol.h"
#include "Tools/FBuild/FBuildCore/FBuildVersion.h"
#include "Tools/FBuild/FBuildCore/FLog.h"

// Core
#include "Core/Env/Env.h"
#include "Core/FileIO/FileIO.h"
#include "Core/FileIO/FileStream.h"
#include "Core/FileIO/PathUtils.h"
#include "Core/Network/Network.h"
#include "Core/Profile/Profile.h"
#include "Core/Strings/AStackString.h"
#include "Core/Process/Thread.h"

// CONSTRUCTOR
//------------------------------------------------------------------------------
WorkerBrokerage::WorkerBrokerage()
    : m_Availability( false )
    , m_Initialized( false )
    , m_SettingsTime( 0 )
{
}

// Init
//------------------------------------------------------------------------------
void WorkerBrokerage::Init()
{
    PROFILE_FUNCTION

    ASSERT( Thread::IsMainThread() );

    if ( m_Initialized )
    {
        return;
    }

    // brokerage path includes version to reduce unnecssary comms attempts
    uint32_t protocolVersion = Protocol::PROTOCOL_VERSION;

    // root folder
    AStackString<> root;
    if ( Env::GetEnvVariable( "FASTBUILD_BROKERAGE_PATH", root ) )
    {
        // <path>/<group>/<version>/
        #if defined( __WINDOWS__ )
            m_BrokerageRoot.Format( "%s\\main\\%u.windows\\", root.Get(), protocolVersion );
        #elif defined( __OSX__ )
            m_BrokerageRoot.Format( "%s/main/%u.osx/", root.Get(), protocolVersion );
        #else
            m_BrokerageRoot.Format( "%s/main/%u.linux/", root.Get(), protocolVersion );
        #endif
    }

    Network::GetHostName(m_HostName);

    AStackString<> filePath;
    m_BrokerageFilePath.Format( "%s%s", m_BrokerageRoot.Get(), m_HostName.Get() );
    m_TimerLastUpdate.Start();

    m_Initialized = true;
}

// DESTRUCTOR
//------------------------------------------------------------------------------
WorkerBrokerage::~WorkerBrokerage()
{
    // Ensure the file disapears when closing
    if ( m_Availability )
    {
        FileIO::FileDelete( m_BrokerageFilePath.Get() );
    }
}

// FindWorkers
//------------------------------------------------------------------------------
void WorkerBrokerage::FindWorkers( Array< AString > & workerList )
{
    PROFILE_FUNCTION

    Init();

    if ( m_BrokerageRoot.IsEmpty() )
    {
        FLOG_WARN( "No brokerage root; did you set FASTBUILD_BROKERAGE_PATH?" );
        return;
    }

    Array< AString > results( 256, true );
    if ( !FileIO::GetFiles( m_BrokerageRoot,
                            AStackString<>( "*" ),
                            false,
                            &results ) )
    {
        FLOG_WARN( "No workers found in '%s'", m_BrokerageRoot.Get() );
        return; // no files found
    }

    // presize
    if ( ( workerList.GetSize() + results.GetSize() ) > workerList.GetCapacity() )
    {
        workerList.SetCapacity( workerList.GetSize() + results.GetSize() );
    }

    // convert worker strings
    const AString * const end = results.End();
    for ( AString * it = results.Begin(); it != end; ++it )
    {
        const AString & fileName = *it;
        const char * lastSlash = fileName.FindLast( NATIVE_SLASH );
        AStackString<> workerName( lastSlash + 1 );
        if ( workerName.CompareI( m_HostName ) != 0 )
        {
            workerList.Append( workerName );
        }
    }
}

// SetAvailability
//------------------------------------------------------------------------------
void WorkerBrokerage::SetAvailability(bool available)
{
    Init();

    // ignore if brokerage not configured
    if ( m_BrokerageRoot.IsEmpty() )
    {
        return;
    }

    if ( available )
    {
        // Check the last update time to avoid too much File IO.
        float elapsedTime = m_TimerLastUpdate.GetElapsedMS();
        if ( elapsedTime >= 10000.0f )
        {
            //
            // Ensure that the file will be recreated if cleanup is done on the brokerage path.
            //
            const WorkerSettings& workerSettings = WorkerSettings::Get();
            const uint64_t currentSettingsTime = workerSettings.GetLastWriteTime();

            if ( !FileIO::FileExists( m_BrokerageFilePath.Get() ) || 
                 ( workerSettings.GetWriteExtraInfoInBrokerFile() && ( currentSettingsTime > m_SettingsTime ) ) )
            {
                FileIO::EnsurePathExists( m_BrokerageRoot );

                // create file to signify availability
                FileStream fs;
                fs.Open( m_BrokerageFilePath.Get(), FileStream::WRITE_ONLY );

                // and write some human readable info in it if needed
                if ( workerSettings.GetWriteExtraInfoInBrokerFile() )
                {
                    AStackString<> line( FBUILD_VERSION_STRING );

                    AStackString<> username;
                    if ( Env::GetEnvVariable( "USERNAME", username ) || Env::GetEnvVariable( "USER", username ) )
                    {
                        line.Append("\n", 1);
                        line.Append(username);
                    }

                    static const uint32_t numProcessors = Env::GetNumProcessors();
                    line.AppendFormat( "\n%u/%u cpus\n", workerSettings.GetNumCPUsToUse(), numProcessors );

                    switch ( workerSettings.GetMode() )
                    {
                        case WorkerSettings::DEDICATED:
                            {
                                line.Append( "dedicated\n", 10 );
                            }
                            break;
                        case WorkerSettings::WHEN_IDLE:
                            {
                                line.AppendFormat( "idle (%u%%)\n", workerSettings.GetIdleThresholdPercent() );
                            }
                            break;
                        case WorkerSettings::DISABLED:
                        default:
                            break;
                    }
                    fs.WriteBuffer( line.Get(), line.GetLength() );
                    m_SettingsTime = currentSettingsTime;
                }

                // Restart the timer
                m_TimerLastUpdate.Start();
            }
        }
    }
    else if ( m_Availability != available )
    {
        // remove file to remove availability
        FileIO::FileDelete( m_BrokerageFilePath.Get() );

        // Restart the timer
        m_TimerLastUpdate.Start();
    }
    m_Availability = available;
}

//------------------------------------------------------------------------------
