#include <QDir>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>
#include <vector>
#include <stdexcept>
#include "globals.h"
#include "settings.h"
#include "receivers.h"

MessageReceiver::MessageReceiver(QObject *parent) noexcept : QObject(parent)
{
	connect(this,&MessageReceiver::Succeeded,this,&MessageReceiver::deleteLater);
	connect(this,&MessageReceiver::Failed,this,&MessageReceiver::deleteLater);
}

void StatusReceiver::Interpret(const QString text)
{
	emit Print("> "+text);
}

void AuthenticationReceiver::Process(const QString data)
{
	if (data.contains(IRC_VALIDATION_AUTHENTICATION))
	{
		emit Print("Server accepted authentication");
		Interpret(data);
		emit Succeeded();
	}
	else
	{
		emit Print("Server did not accept authentication");
		Interpret(data);
		emit Failed();
	}
}

ChannelJoinReceiver::ChannelJoinReceiver(QObject *parent) noexcept  : StatusReceiver(parent)
{
	failureDelay.setSingleShot(true);
	failureDelay.setInterval(1000); // TODO: Make this an advanced setting
	connect(&failureDelay,&QTimer::timeout,this,&ChannelJoinReceiver::Fail);
}

void ChannelJoinReceiver::Process(const QString data)
{
	if (data.contains(IRC_VALIDATION_JOIN))
	{
		if (failureDelay.isActive()) failureDelay.stop();
		emit Print("Stream joined");
		Interpret(data);
		emit Succeeded();
	}
	else
	{
		if (!failureDelay.isActive()) failureDelay.start();
		Interpret(data);
	}
}

void ChannelJoinReceiver::Fail()
{
	emit Print("Failed to join channel for stream");
	emit Failed();
}

ChatMessageReceiver::ChatMessageReceiver(std::vector<Command> builtInCommands,QObject *parent) : MessageReceiver(parent)
{
	QDir commandListPath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
	if (!commandListPath.mkpath(commandListPath.absolutePath())) throw std::runtime_error(QString("Failed to create command list path: %1").arg(commandListPath.absolutePath()).toStdString());
	QFile commandListFile(commandListPath.filePath(COMMANDS_LIST_FILENAME));
	if (!commandListFile.open(QIODevice::ReadWrite)) throw std::runtime_error(QString("Failed to open command list file: %1").arg(commandListFile.fileName()).toStdString());

	QJsonParseError jsonError;
	QByteArray data=commandListFile.readAll();
	if (data.isEmpty()) data="[]";
	QJsonDocument json=QJsonDocument::fromJson(data,&jsonError);
	if (json.isNull()) throw std::runtime_error(jsonError.errorString().toStdString());
	for (const QJsonValue jsonValue : json.array())
	{
		QJsonObject jsonObject=jsonValue.toObject();
		const QString name=jsonObject.value(JSON_KEY_COMMAND_NAME).toString();
		commands[name]={
			name,
			jsonObject.value(JSON_KEY_COMMAND_DESCRIPTION).toString(),
			COMMAND_TYPES.at(jsonObject.value(JSON_KEY_COMMAND_TYPE).toString()),
			jsonObject.contains(JSON_KEY_COMMAND_RANDOM_PATH) ? jsonObject.value(JSON_KEY_COMMAND_RANDOM_PATH).toBool() : false,
			jsonObject.value(JSON_KEY_COMMAND_PATH).toString(),
			jsonObject.contains(JSON_KEY_COMMAND_MESSAGE) ? jsonObject.value(JSON_KEY_COMMAND_MESSAGE).toString() : QString()
		};
	}
	for (const Command &command : builtInCommands) commands[command.Name()]=command;
	for (const std::pair<QString,Command> command : commands)
	{
		if (!command.second.Protect()) userCommands.push_back(command.second);
	}
}

void ChatMessageReceiver::AttachCommand(const Command &command)
{
	commands[command.Name()]=command;
}

const Command ChatMessageReceiver::RandomCommand() const
{
	return userCommands[QRandomGenerator::global()->bounded(0,userCommands.size())];
}

void ChatMessageReceiver::Process(const QString data)
{
	QStringList messageSegments=data.split(":",Qt::SkipEmptyParts);
	if (messageSegments.size() < 2)
	{
		emit Failed();
		return;
	}
	QString hostmask=QString(messageSegments.front());
	messageSegments.pop_front();
	QStringList hostmaskSegments=hostmask.split("!",Qt::SkipEmptyParts);
	if (hostmaskSegments.size() < 1)
	{
		emit Failed();
		return;
	}
	QString user=hostmaskSegments.front();
	IdentifyViewer(user);
	if (messageSegments.at(0).at(0) == "!")
	{
		QStringList commandSegments=messageSegments.at(0).trimmed().split(" ");
		QString commandName=commandSegments.at(0).mid(1);
		commandSegments.pop_front();
		QString parameter=commandSegments.join(" ");
		if (commands.find(commandName) != commands.end())
		{
			Command command=commands.at(commandName);
			if (command.Protect() && settingAdministrator != user)
			{
				emit Alert(QString("The command %1 is protected but %2 is not the broadcaster").arg(command.Name(),user));
				return;
			}
			switch (command.Type())
			{
			case CommandType::VIDEO:
				if (command.Random())
				{
					QDir directory(command.Path());
					QStringList videos=directory.entryList(QStringList() << "*.mp4",QDir::Files);
					if (videos.size() < 1)
					{
						Print("No videos found");
						break;
					}
					emit PlayVideo(directory.absoluteFilePath(videos[QRandomGenerator::global()->bounded(0,videos.size())]));
				}
				else
				{
					emit PlayVideo(command.Path());
				}
				break;
			case CommandType::AUDIO:
				emit PlayAudio(user,command.Message(),command.Path());
				break;
			case CommandType::DISPATCH:
				DispatchCommand({command,parameter});
				break;
			};
		}
	}
	emit Print(QString("<div class='user'>%1</div><div class='message'>%2</div><br>").arg(user,messageSegments.join(":")));
}

void ChatMessageReceiver::IdentifyViewer(const QString &name)
{
	Viewer viewer(name);
	if (viewers.find(name) == viewers.end()) emit ArrivalConfirmed(name);
	viewers.emplace(name,viewer);
}
