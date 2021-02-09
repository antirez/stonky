Stonky is a Telegram bot that provides access to financial informations.
It is backed by the publicly available Yahoo Finance HTTP API, it is written
in C and is released under the BSD license.

To use it:

1. Create your bot using the Telegram [@BotFather](https://t.me/botfather).
2. After obtaining your bot API key, store it into a file called `apikey.txt` inside the bot working directory. Alternatively you can use the `--apikey` command line argument to provide your Telegram API key.
3. Build Stonky: you need libcurl and libsqlite installed. Just type `make`.
4. Run Stonky with `./stonky`. There is also a debug mode if you run it using the `--debug` option.
5. Add the bot to your Telegram channel.
6. **IMPORTANT:** The bot *must* be an administrator of the channel in order to read all the messages that are sent in such channel.

To use the bot, send messages in a Telegram channel where the bot is an
administrator. All the Stonky commands start with the "$" character.

    $AAPL           -- Reply with an update about AAPL price.
    $AAPL 1y        -- Reply with an ASCII art graph of AAPL price during
                       the past year. Valid intervals are: 1d|5d|1m|6m|1y
    $AAPL mc        -- Reply with the result of a Montecarlo simulation on
                       the 52 weeks price of the specified stock. You can
                       say "montecarlo" instead of "mc" if you want.

The bot supports the concept of "list of stocks", you can add stocks to
a list, then query the list to have all the prices with a single message:

    $mylist: +VMW +AAPL +T -KO  -- Modify the list adding/removing stocks.
    $mylist:                    -- Ask prices of stocks in a given list.


