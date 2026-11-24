/*
 * puddlejumper — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

// Notifier on macOS: the notification centre (UserNotifications). Manual
// reference counting, as macwindow.mm.

#include "notifier.h"

#include <QPointer>

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

namespace {

// The notification centre is only there for an app bundle; asked for from a
// bare binary (a build run from its folder) it raises an exception.
bool haveCentre()
{
    NSBundle *bundle = [NSBundle mainBundle];
    return bundle.bundleIdentifier != nil && [bundle.bundlePath hasSuffix:@".app"];
}

} // namespace

@interface PJNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@property (nonatomic) QPointer<Notifier> notifier;
@end

@implementation PJNotificationDelegate

// Shown even while the app is in front: it's about a tab not being looked at.
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
{
    Q_UNUSED(center);
    Q_UNUSED(notification);
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList);
}

- (void)userNotificationCenter:(UNUserNotificationCenter *)center
    didReceiveNotificationResponse:(UNNotificationResponse *)response
             withCompletionHandler:(void (^)(void))completionHandler
{
    Q_UNUSED(center);
    const QString key = QString::fromNSString(response.notification.request.identifier);
    QPointer<Notifier> notifier = self.notifier;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (notifier && [response.actionIdentifier isEqualToString:UNNotificationDefaultActionIdentifier])
            emit notifier->activated(key);
    });
    completionHandler();
}

@end

Notifier::Notifier(QObject *parent)
    : QObject(parent)
{
    if (!haveCentre())
        return;
    PJNotificationDelegate *delegate = [[PJNotificationDelegate alloc] init];   // kept: released below
    delegate.notifier = this;
    UNUserNotificationCenter.currentNotificationCenter.delegate = delegate;
    m_delegate = delegate;
}

Notifier::~Notifier()
{
    if (!m_delegate)
        return;
    UNUserNotificationCenter.currentNotificationCenter.delegate = nil;
    PJNotificationDelegate *delegate = static_cast<PJNotificationDelegate *>(m_delegate);
    delegate.notifier = nullptr;
    [delegate release];
}

void Notifier::notify(const QString &key, const QString &title, const QString &body)
{
    if (!m_delegate)
        return;
    UNUserNotificationCenter *centre = UNUserNotificationCenter.currentNotificationCenter;
    NSString *identifier = key.toNSString();
    NSString *heading = title.toNSString();
    NSString *text = body.toNSString();
    // Asked the first time; after that macOS remembers the answer.
    [centre requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                          completionHandler:^(BOOL granted, NSError *error) {
        Q_UNUSED(error);
        if (!granted)
            return;
        UNMutableNotificationContent *content = [[[UNMutableNotificationContent alloc] init] autorelease];
        content.title = heading;
        content.body = text;
        content.threadIdentifier = identifier;
        // The same identifier replaces the tab's earlier notification.
        UNNotificationRequest *request = [UNNotificationRequest requestWithIdentifier:identifier
                                                                              content:content
                                                                              trigger:nil];
        [centre addNotificationRequest:request withCompletionHandler:nil];
    }];
}

void Notifier::withdraw(const QString &key)
{
    if (!m_delegate)
        return;
    NSArray<NSString *> *identifiers = @[ key.toNSString() ];
    UNUserNotificationCenter *centre = UNUserNotificationCenter.currentNotificationCenter;
    [centre removeDeliveredNotificationsWithIdentifiers:identifiers];
    [centre removePendingNotificationRequestsWithIdentifiers:identifiers];
}
