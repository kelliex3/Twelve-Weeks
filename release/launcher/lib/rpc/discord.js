/**
 *  @file The Discord RPC master class
 */

/**
 * Creates a Discord RPC adapter.
 * @param {{ config: object, log: Function, player: object }} options
 * @returns {import('./noop.js').RpcAdapter}
 */
'use strict';
const { Client } = require('@xhayper/discord-rpc');

function createDiscordAdapter({ config, log, player }) {
  let client = null;
  let connected = false;
  let connecting = null;
  const clientId = config.values.rpc?.discord?.clientId || '1569450156942819470';
  /**
   * Main connection callback for the Discord RPC
   */
  async function connect() {
    if (!clientId) return;
    if (connected) return;
    if (connecting) return connecting;
    connecting = (async () => {
      try {
        client = new Client({ clientId });
        client.on('disconnected', () => {
          // disconnected
          log('Discord has disconnected.');
          connected = false;
          client = null;
        });
        await client.login();
        connected = true;
        log('Discord RPC successfully connected!');
      } catch (e) {
        // something went wrong
        log(`Discord RPC has been disabled due to the following error: ${e.message}`);
        connected = false;
        client = null;
      } finally {
        connecting = null; // allow a later activity to retry
      }
    })();
    return connecting;
  }

  /**
   * Maps a generic RPC activity to Discord
   * @param {import('./noop.js').RpcActivity} activity A generic RPC activity
   * @returns {import('@xhayper/discord-rpc').SetActivity} An activity ready for consumption by Discord
   */
  function mapActivity(activity) {
    return {
      // the in-game card (./ingame.js) names the game itself; the launcher's own cards keep the launcher name
      name: activity.name || 'Twelve Weeks - Launcher', // nicely generic because Discord doesn't allow i18n for this
      type: 0, // ActivityType.Playing (the package does not re-export the enum)
      state: activity.state,
      details: activity.details,
      startTimestamp: activity.timestamps?.start || Math.floor(Date.now() / 1000),
      partyId: activity.partyId,
      partySize: activity.partySize,
      partyMax: activity.partyMax,
      largeImageKey: activity.assets?.largeImage || 'https://twelve-weeks.com/assets/rpc/logo-bw.png',
      largeImageText: activity.assets?.largeText,
      smallImageKey: activity.assets?.smallImage,
      smallImageText: activity.assets?.smallText,
      // the in-game card (./ingame.js) links the project site; the launcher's own cards have none
      buttons: activity.buttons?.length ? activity.buttons.slice(0, 2) : undefined,
    };
  }

  /**
   * Activity setter for the Discord RPC
   * @param {import('./noop.js').RpcActivity} activity The generic activity object
   * @returns {import('./noop.js').RpcAdapter} An adapter object
   */
  async function setActivity(activity) {
    if (!clientId) return;
    await connect();
    if (!client || !connected || !client.user) return;
    try {
      await client.user.setActivity(mapActivity(activity));
      log(`Discord RPC activity set: ${activity.details || activity.state || ''}`);
    } catch (e) {
      log(`Discord RPC setActivity failed because of ${e.message}!`);
    }
  }
  /**
   * Clears the current activity
   */
  async function clear() {
    if (!client || !connected || !client.user) return;
    try {
      await client.user.clearActivity();
    } catch (e) {
      log(`Discord RPC clear failed: ${e.message}`);
    }
  }

  /**
   * Teardown of Discord RPC stuff
   */
  async function destroy() {
    const c = client;
    client = null;
    connected = false;
    if (!c) return;
    try {
      await c.destroy();
    } catch (e) {
      log(`Discord RPC teardown failed: ${e.message}`);
    }
  }

  return {
    name: 'discord',
    get enabled() {
      return !!clientId;
    },
    get isReady() {
      return connected;
    },
    connect: connect,
    setActivity: setActivity,
    clear: clear,
    destroy: destroy,
  };
}

module.exports = { createDiscordAdapter };
