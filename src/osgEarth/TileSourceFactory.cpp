/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2009 Pelican Ventures, Inc.
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include <osgEarth/TileSourceFactory>
#include <osgEarth/Caching>
#include <osgEarth/DirectReadTileSource>
#include <osgEarth/Registry>

using namespace osgEarth;

TileSource*
TileSourceFactory::createMapTileSource( MapLayer* layer, Map* map )
{
    /*
    *The map tile source chain will look like
    *MemCachedTileSource -> DiskCachedTileSource -> TileSource
    *
    *If no cache is configured, the chain will look like
    *MemCachedTileSource -> TileSource
    *
    *If the cache only option is specified, the chain will look like
    *MemCachedTileSource -> DiskCachedTileSource
    */

    osg::notify(osg::INFO) << "[osgEarth] Creating TileSource for '" << layer->getName() << "':" << std::endl;

    const osgDB::ReaderWriter::Options* global_options = map->getGlobalOptions();

    osg::ref_ptr<osgDB::ReaderWriter::Options> local_options = global_options ?
        new osgDB::ReaderWriter::Options( *global_options ) : 
        new osgDB::ReaderWriter::Options();

    //Setup the plugin options for the source
    for( Properties::const_iterator p = layer->getDriverProperties().begin(); p != layer->getDriverProperties().end(); p++ )
    {
        local_options->setPluginData( p->first, (void*)p->second.c_str() );
    }

    bool foundValidSource = false;
    osg::ref_ptr<TileSource> tile_source;

    //Configure the cache if necessary
    optional<CacheConfig> layerCacheConf = layer->cacheConfig();

    bool cacheOnlyEnv = false;
    //If the OSGEARTH_CACHE_ONLY environment variable is set, override whateve is in the map config
    if (getenv("OSGEARTH_CACHE_ONLY") != 0)
    {
        osg::notify(osg::NOTICE) << "[osgEarth::TileSourceFactory] Setting osgEarth to cache only mode due to OSGEARTH_CACHE_ONLY environment variable " << std::endl;
        cacheOnlyEnv = true;
    }

    const optional<CacheConfig>& mapCacheConf = map->cacheConfig();

    bool runOffCacheOnly =
        cacheOnlyEnv ||
        ( mapCacheConf.isSet() && mapCacheConf->runOffCacheOnly().isSet() && mapCacheConf->runOffCacheOnly() == true ) ||
        ( layerCacheConf.isSet() && layerCacheConf->runOffCacheOnly().isSet() && layerCacheConf->runOffCacheOnly() == true );

    //Only load the source if we are not running offline
    if ( !runOffCacheOnly )
    {
        //Add the source to the list.  The "." prefix causes OSG to select the correct plugin.
        //For instance, the WMS plugin can be loaded by using ".osgearth_wms" as the filename
        tile_source = dynamic_cast<TileSource*>(
            osgDB::readObjectFile( ".osgearth_" + layer->getDriver(), local_options.get()));

        if ( !tile_source.valid() )
        {
            osg::notify(osg::NOTICE) << "[osgEarth] Warning: Could not load TileSource for driver "  << layer->getDriver() << std::endl;
        }
    }

    // Inherit from the MapConfig if it is defined
    if ( mapCacheConf.isSet() )
    {
        if ( layerCacheConf.isSet() )
            layerCacheConf->inheritFrom( mapCacheConf.get() );
        else
            layerCacheConf = mapCacheConf.get();

        osg::notify(osg::INFO) << "[osgEarth] Layer '" << layer->getName() << "': Inheriting cache configuration from map..." << std::endl;
    }

    // Inherit the map CacheConfig with the override from the registry
    if ( Registry::instance()->cacheConfigOverride().isSet() )
    {
        if ( layerCacheConf.isSet() )
            layerCacheConf->inheritFrom( Registry::instance()->cacheConfigOverride().get() );
        else
            layerCacheConf = Registry::instance()->cacheConfigOverride().get();

        osg::notify(osg::NOTICE) << "[osgEarth] Layer '" << layer->getName() << "': Applying global cache override" << std::endl;
    }

    if ( tile_source.valid() )
    {           
        //Initialize the source and set its name
        tile_source->setName( layer->getName() );
        osg::notify(osg::INFO) << "[osgEarth] Layer '" << layer->getName() << "': created TileSource for driver " << layer->getDriver() << std::endl;

		//If the TileSource is set to reproject before caching occurs, wrap the TileSource in a DirectReadTileSource.
		//This allows us to pull down imagery from pretiled datasources such as TMS or WMS-C that may not be in
		//the correct SRS, reproject them to the cache, and have a nice fast map once caching occurs.
		if ( layerCacheConf.isSet() && layerCacheConf->reprojectBeforeCaching() == true ) //layer->getReprojectBeforeCaching() )
		{			
			//Try to read in a preferred tile_size.
            int tile_size = as<int>( layer->getDriverProperties().get("tile_size"), 256 );
			osg::notify(osg::INFO) << "[osgEarth] Layer '" << layer->getName() << "': wrapping in DirectReadTileSource with a tile size of " << tile_size << std::endl;
			tile_source = new DirectReadTileSource( tile_source, tile_size );
		}
    }

    osg::ref_ptr<TileSource> topSource = tile_source.get();

    //If the cache config is valid, wrap the TileSource with a caching TileSource.
    if ( layerCacheConf.isSet() && layerCacheConf->getType() != CacheConfig::TYPE_DISABLED ) // && tile_source->supportsPersistentCaching() )
    {
        osg::ref_ptr<CachedTileSource> cache = CachedTileSourceFactory::create(
            tile_source.get(),
            layerCacheConf->getType(),
            layerCacheConf->getProperties(),
            local_options.get() );

        if (cache.valid())
        {
            cache->setName( layer->getName() );
            cache->setMapConfigFilename( map->getReferenceURI() );
            topSource = cache.get();

            osg::notify(osg::INFO)
                << "[osgEarth] Layer '" << layer->getName() << "': Cache setup: " 
                << (layerCacheConf.isSet() ? layerCacheConf->toString() : "unset")
                << std::endl;
        }
    }
    else
    {
        osg::notify(osg::INFO) << "[osgEarth] Layer '" << layer->getName() << "': Caching disabled" << std::endl;
    }

    MemCachedTileSource* memCachedSource = new MemCachedTileSource(topSource.get(), local_options.get());
    memCachedSource->setName( topSource->getName() );

    // Finally, install an override profile if the caller requested one. This will override the profile
    // that the TileSource reports.
    if ( layer->profileConfig().isSet() )
    {
        const ProfileConfig& pconf = layer->profileConfig().get();

        osg::ref_ptr<const Profile> override_profile;

        if ( !pconf.getNamedProfile().empty() )
        {
            override_profile = osgEarth::Registry::instance()->getNamedProfile( pconf.getNamedProfile() );
        }

        if ( !override_profile.valid() && !pconf.getSRS().empty() )
        {
            double xmin, ymin, xmax, ymax;
            pconf.getExtents( xmin, ymin, xmax, ymax );
            override_profile = Profile::create( pconf.getSRS(), xmin, ymin, xmax, ymax );
        }

        if ( override_profile.valid() )
        {
            memCachedSource->setOverrideProfile( override_profile.get() );
            osg::notify(osg::INFO) << "  Applying profile override" << std::endl;
        }
    }

    return memCachedSource;
}

TileSource*
TileSourceFactory::createDirectReadTileSource(MapLayer* layer,
                                              const Profile* profile,
                                              unsigned int tileSize,
                                              const osgDB::ReaderWriter::Options* global_options)
{
    //Create the map source
    osg::ref_ptr<Map> map = new Map();
    map->setGlobalOptions( global_options );
    TileSource* source = createMapTileSource( layer, map.get() );

    //MapConfig mapConfig;
    //mapConfig.setGlobalOptions( global_options );
    //TileSource* source = createMapTileSource( sourceConfig, mapConfig );

    //Wrap it in a DirectTileSource that will convert to the map's profile
    TileSource* tileSource = new DirectReadTileSource( source, tileSize, global_options );
    tileSource->initProfile( profile, std::string() );
    tileSource->setName( layer->getName() );

    return tileSource;
}