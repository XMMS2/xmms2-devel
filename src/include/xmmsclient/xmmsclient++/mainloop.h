/*  XMMS2 - X Music Multiplexer System
 *  Copyright (C) 2003-2023 XMMS2 Team
 *
 *  PLUGINS ARE NOT CONSIDERED TO BE DERIVED WORK !!!
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */

#ifndef XMMSCLIENTPP_MAINLOOP_H
#define XMMSCLIENTPP_MAINLOOP_H

#include <xmmsclient/xmmsclient++/listener.h>

#include <list>


namespace Xmms 
{

	/** @class MainloopInterface mainloop.h "xmmsclient/xmmsclient++/mainloop.h"
	 *  @brief Interface to all mainloops run by Client.
	 */
	class MainloopInterface
	{

		public:

			/** Constructor.
			 *
			 *  @param conn xmmsc_connection_t* of the connection to
			 *              use in the mainloop.
			 *
			 *  @note The constructor should only initialize the
			 *        mainloop, not start it!
			 */
			MainloopInterface( xmmsc_connection_t*& conn ) :
				running_( false ), conn_( conn ) { }

			/** Destructor.  Should also stop the loop.
			 */
			virtual ~MainloopInterface() { }

			/** Start the mainloop.
			 *
			 * @note Some mainloop implementations might require the
			 *       use of a separate function to be run.
			 */
			virtual void run() = 0;

			/** Check if the mainloop is currently running.
			 */
			bool isRunning() const {
				return running_;
			}

		protected:
			bool running_;
			xmmsc_connection_t*& conn_;

	};

	/** @class MainLoop mainloop.h "xmmsclient/xmmsclient++/mainloop.h"
	 *  @brief A standalone mainloop allowing additional custom Listeners.
	 *  Can only be created by the Client class.
	 */
	class MainLoop : public MainloopInterface
	{

		public:
			/** Copy-constructor.
			 */
			MainLoop( const MainLoop& src );

			/** Destructor.
			 *
			 * @note The destructors frees all the registered listeners.
			 */
			virtual ~MainLoop();

			/** Add a new listener to listen on in the loop.
			 */
			void addListener( ListenerInterface* l );

			/** Remove an existing listener from the loop.
			 *
			 * @note The ListenerInterface comparison operator is used
			 *       (i.e. compare the file descriptors), not pointer
			 *       comparison.
			 */
			void removeListener( ListenerInterface* l );

			/** Start the mainloop.
			 *  Only stops if the connection is lost or all listeners
			 *  are removed.
			 */
			virtual void run();

		/** @cond */
		private:

			// Constructor, only to be called by Xmms::Client
			friend class Client;

			/** Constructor, only callable by Client.
			 *
			 *  @param conn xmmsc_connection_t* of the connection used
			 *              in the mainloop.
			 */
			MainLoop( xmmsc_connection_t*& conn );

			/** Copy assignment operator, only callable by Client.
			 */
			MainLoop& operator=( const MainLoop& src ) const;

			/** List of listeners listened on.
			 */
			std::list< ListenerInterface* > listeners;

			/** The method actually doing the io operations on the
			 *  listeners.
			 */
			void waitForData();

		/** @endcond */
	};

}

#endif // XMMSCLIENTPP_MAINLOOP_H
