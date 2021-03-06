#include <QEvent>
#include <QTimer>
#include <QLayout>
#include <QGridLayout>
#include <QRandomGenerator>
#include <chrono>
#include <stdexcept>
#include "window.h"
#include "volume.h"
#include "receivers.h"

std::chrono::milliseconds Window::uptime=TimeConvert::Now();

Window::Window() : QWidget(nullptr),
	ircSocket(nullptr),
	visiblePane(nullptr),
	background(new QWidget(this)),
	vibeKeeper(new QMediaPlayer(this)),
	settingHelpCooldown(SETTINGS_CATEGORY_WINDOW,"HelpCooldown",300000),
	settingVibePlaylist(SETTINGS_CATEGORY_VIBE,"Playlist"),
	settingOAuthToken(SETTINGS_CATEGORY_AUTHORIZATION,"Token"),
	settingJoinDelay(SETTINGS_CATEGORY_AUTHORIZATION,"JoinDelay",5),
	settingBackgroundColor(SETTINGS_CATEGORY_WINDOW,"BackgroundColor","#ff000000"),
	settingAccentColor(SETTINGS_CATEGORY_WINDOW,"AccentColor","#ff000000"),
	settingArrivalSound(SETTINGS_CATEGORY_EVENTS,"Arrival",""),
	settingThinkingSound(SETTINGS_CATEGORY_EVENTS,"Thinking","")
{
	setAttribute(Qt::WA_TranslucentBackground,true);
	setFixedSize(537,467);

	setLayout(new QGridLayout(this));
	layout()->setContentsMargins(0,0,0,0);
	background->setLayout(new QGridLayout(background)); // for translucency to work, there has to be a widget covering the window, otherwise the entire thing is clear
	background->layout()->setContentsMargins(0,0,0,0);
	QColor backgroundColor=settingBackgroundColor;
	background->setStyleSheet(QString("background-color: rgba(%1,%2,%3,%4);").arg(
		StringConvert::Integer(backgroundColor.red()),
		StringConvert::Integer(backgroundColor.green()),
		StringConvert::Integer(backgroundColor.blue()),
		StringConvert::Integer(backgroundColor.alpha())));
	layout()->addWidget(background);

	SwapPane(new StatusPane(this));

	helpClock.setInterval(TimeConvert::Interval(std::chrono::milliseconds(settingHelpCooldown)));
	vibeKeeper->setVolume(0);

	connect(this,&Window::Ponging,this,&Window::Pong);
}

Window::~Window()
{
	ircSocket->disconnectFromHost();
}

bool Window::event(QEvent *event)
{
	if (event->type() == QEvent::Polish)
	{
		ircSocket=new QTcpSocket(this);
		connect(ircSocket,&QTcpSocket::connected,this,&Window::Connected);
		connect(ircSocket,&QTcpSocket::readyRead,this,&Window::DataAvailable);
		Authenticate();
	}

	return QWidget::event(event);
}

void Window::Connected()
{
	emit Print("Connected!");
	Print(QString(IRC_COMMAND_USER).arg(static_cast<QString>(settingAdministrator)));
	emit Print("Sending credentials...");
	ircSocket->write(QString(IRC_COMMAND_PASSWORD).arg(static_cast<QString>(settingOAuthToken)).toLocal8Bit());
	ircSocket->write(QString(IRC_COMMAND_USER).arg(static_cast<QString>(settingAdministrator)).toLocal8Bit());
}

void Window::Disconnected()
{
	emit Print("Disconnected");
}

void Window::DataAvailable()
{
	QString data=ircSocket->readAll();
	if (data.size() > 4 && data.left(4) == "PING")
	{
		emit Ponging();
		return;
	}
	emit Dispatch(data);
}

void Window::SwapPane(Pane *pane)
{
	if (visiblePane) visiblePane->deleteLater();
	visiblePane=pane;
	background->layout()->addWidget(visiblePane);
	connect(this,&Window::Print,visiblePane,&Pane::Print);
}

void Window::Authenticate()
{
	AuthenticationReceiver *authenticationReceiver=new AuthenticationReceiver(this);
	connect(this,&Window::Dispatch,authenticationReceiver,&AuthenticationReceiver::Process);
	connect(authenticationReceiver,&AuthenticationReceiver::Print,visiblePane,&Pane::Print);
	connect(authenticationReceiver,&AuthenticationReceiver::Succeeded,[this]() {
		Print(QString("Joining stream in %1 seconds...").arg(TimeConvert::Interval(TimeConvert::Seconds(settingJoinDelay))));
		QTimer::singleShot(TimeConvert::Interval(static_cast<std::chrono::milliseconds>(settingJoinDelay)),this,&Window::JoinStream);
	});
	ircSocket->connectToHost(TWITCH_HOST,TWITCH_PORT);
	emit Print("Connecting to IRC...");
}

void Window::JoinStream()
{
	ChannelJoinReceiver *channelJoinReceiver=new ChannelJoinReceiver(this);
	connect(this,&Window::Dispatch,channelJoinReceiver,&ChannelJoinReceiver::Process);
	connect(channelJoinReceiver,&ChannelJoinReceiver::Print,visiblePane,&Pane::Print);
	connect(channelJoinReceiver,&ChannelJoinReceiver::Succeeded,this,&Window::FollowChat);
	ircSocket->write(StringConvert::ByteArray(IRC_COMMAND_JOIN.arg(static_cast<QString>(settingAdministrator))));
}

void Window::FollowChat()
{
	ChatMessageReceiver *chatMessageReceiver=nullptr;
	try
	{
		chatMessageReceiver=new ChatMessageReceiver({
			AgendaCommand,PingCommand,SongCommand,ThinkCommand,UptimeCommand,VibeCommand,VolumeCommand
		},this);
	}
	catch (const std::runtime_error &exception)
	{
		visiblePane->Print(QString("There was a problem starting chat\n%1").arg(exception.what()));
		return;
	}

	ChatPane *chatPane=new ChatPane(this);
	connect(this,&Window::Ponging,[chatPane]() {
		chatPane->Alert("Received PING, sending PONG");
	});
	connect(chatMessageReceiver,&ChatMessageReceiver::Alert,chatPane,&ChatPane::Alert);
	SwapPane(chatPane);

	connect(this,&Window::Dispatch,chatMessageReceiver,&ChatMessageReceiver::Process);
	connect(chatMessageReceiver,&ChatMessageReceiver::Print,visiblePane,&Pane::Print);
	connect(chatMessageReceiver,&ChatMessageReceiver::ArrivalConfirmed,this,[this](const Viewer &viewer) {
		if (settingAdministrator != viewer.Name()) StageEphemeralPane(new AudioAnnouncePane(QString("Please welcome %1 to the chat.").arg(viewer.Name()),settingArrivalSound));
	});
	connect(chatMessageReceiver,&ChatMessageReceiver::PlayVideo,[this](const QString &path) {
		StageEphemeralPane(new VideoPane(path));
	});
	connect(chatMessageReceiver,&ChatMessageReceiver::PlayAudio,[this](const QString &user,const QString &message,const QString &path) {
		StageEphemeralPane(new AudioAnnouncePane(QString("**%1**\n\n%2").arg(user,message),path));
	});

	connect(chatMessageReceiver,&ChatMessageReceiver::DispatchCommand,[this,chatPane](const Command &command) {
		try
		{
			switch (BUILT_IN_COMMANDS.at(command.Name()))
			{
			case BuiltInCommands::AGENDA:
				chatPane->SetAgenda(command.Message());
				break;
			case BuiltInCommands::PING:
				ircSocket->write(StringConvert::ByteArray(TWITCH_PING));
				break;
			case BuiltInCommands::SONG:
			{
				ImageAnnouncePane *pane=Tuple::New<ImageAnnouncePane,QString,QImage>(CurrentSong());
				pane->AccentColor(settingAccentColor);
				StageEphemeralPane(pane);
				break;
			}
			case BuiltInCommands::THINK:
				StageEphemeralPane(new AudioAnnouncePane("Shh... Herb is thinking...",settingThinkingSound));
				break;
			case BuiltInCommands::UPTIME:
			{
				Relay::Status::Context *statusContext=chatPane->Alert(Uptime());
				connect(statusContext,&Relay::Status::Context::UpdateRequested,[this,statusContext]() {
					emit statusContext->Updated(Uptime());
				});
				break;
			}
			case BuiltInCommands::VIBE:
				if (vibeKeeper->state() == QMediaPlayer::PlayingState)
				{
					chatPane->Alert("Pausing the vibes...");
					vibeKeeper->pause();
					break;
				}
				vibeKeeper->play();
				break;
			case BuiltInCommands::VOLUME:
				try
				{
					Volume::Fader *fader=new Volume::Fader(vibeKeeper,command.Message(),this);
					connect(fader,&Volume::Fader::Feedback,chatPane,&ChatPane::Alert);
					fader->Start();
				}
				catch (const std::range_error &exception)
				{
					chatPane->Alert(QString("%1\n\n%2").arg("Failed to adjust volume!",exception.what()));
				}
				break;
			}
		}
		catch (const std::out_of_range &exception)
		{
			chatPane->Alert(QString(R"(Command "%1" not fully implemented!)").arg(command.Name()));
		}
	});

	if (settingVibePlaylist)
	{
		connect(&vibeSources,&QMediaPlaylist::loadFailed,[this,chatPane]() {
			chatPane->Alert(QString("**Failed to load vibe playlist**\n\n%2").arg(vibeSources.errorString()));
		});
		connect(&vibeSources,&QMediaPlaylist::loaded,[this,chatPane]() {
			vibeSources.shuffle();
			vibeSources.setCurrentIndex(QRandomGenerator::global()->bounded(0,vibeSources.mediaCount()));
			vibeKeeper->setPlaylist(&vibeSources);
			chatPane->Alert("Vibe playlist loaded!");
		});
		vibeSources.load(QUrl::fromLocalFile(settingVibePlaylist));
		connect(vibeKeeper,QOverload<QMediaPlayer::Error>::of(&QMediaPlayer::error),[this,chatPane](QMediaPlayer::Error error) {
			chatPane->Alert(QString("**Vibe Keeper failed to start**\n\n%2").arg(vibeKeeper->errorString()));
		});
		connect(vibeKeeper,&QMediaPlayer::stateChanged,[this,chatPane](QMediaPlayer::State state) {
			if (state == QMediaPlayer::PlayingState) chatPane->Alert(std::get<QString>(CurrentSong()));
		});
	}

	connect(&helpClock,&QTimer::timeout,[this,chatMessageReceiver]() {
		if (!ephemeralPanes.empty()) return;
		const Command &command=chatMessageReceiver->RandomCommand();
		StageEphemeralPane(new AnnouncePane(QString("!%1\n\n%2").arg(
			command.Name(),
			command.Description()
		)));
	});
	helpClock.start();
}

void Window::StageEphemeralPane(EphemeralPane *pane)
{
	connect(pane,&EphemeralPane::Finished,this,&Window::ReleaseLiveEphemeralPane);
	background->layout()->addWidget(pane);
	if (ephemeralPanes.size() > 0)
	{
		connect(ephemeralPanes.back(),&EphemeralPane::Finished,pane,&EphemeralPane::Show);
		ephemeralPanes.push(pane);
	}
	else
	{
		ephemeralPanes.push(pane);
		visiblePane->hide();
		pane->Show();
	}
}

void Window::ReleaseLiveEphemeralPane()
{
	if (ephemeralPanes.empty()) throw std::logic_error("Ran out of ephemeral panes but messages still coming in to remove them"); // FIXME: this is undefined behavior since it's being thrown from a slot
	ephemeralPanes.pop();
	if (ephemeralPanes.empty()) visiblePane->show();
}

std::tuple<QString,QImage> Window::CurrentSong() const
{
	return {
		QString("Now playing %1 by %2\n\nfrom the ablum %3").arg(
			vibeKeeper->metaData("Title").toString(),
			vibeKeeper->metaData("AlbumArtist").toString(),
			vibeKeeper->metaData("AlbumTitle").toString()
		),
		vibeKeeper->metaData("CoverArtImage").value<QImage>()
	};
}

void Window::Pong() const
{
	ircSocket->write(StringConvert::ByteArray(TWITCH_PONG));
}

const QString Window::Uptime() const
{
	std::chrono::milliseconds duration=TimeConvert::Now()-uptime;
	std::chrono::hours hours=std::chrono::duration_cast<std::chrono::hours>(duration);
	std::chrono::minutes minutes=std::chrono::duration_cast<std::chrono::minutes>(duration-hours);
	std::chrono::seconds seconds=std::chrono::duration_cast<std::chrono::seconds>(duration-minutes);
	return QString("Connection to Twitch has been live for %1 hours, %2 minutes, and %3 seconds").arg(StringConvert::Integer(hours.count()),StringConvert::Integer(minutes.count()),StringConvert::Integer(seconds.count()));
}
